// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_RFDEMODULATOR_H
#define MUSECPP_RFDEMODULATOR_H

#include <cstdint>
#include <pthread.h>
#include <fstream>
#include <format>
#include <cassert>
#include <stdexcept>
#include <string_view>
#include <cstdlib>
#include <complex>
#include <array>
#include <algorithm>
#include <thread>
#include <atomic>
#include <deque>
#include <memory>
#include <optional>
#include <utility>
#include <exception>
#include <condition_variable>
#include "logging/Logger.h"
#include "input/InputReader.h"
#include "input/InputReaderFactory.h"
#include "musevk/VulkanManager.h"

template<class B>
class RfDemodulator {
public:
    RfDemodulator(Logger &log, std::string executable_dir, std::string filename, float sample_frequency,
                  musevk::VulkanManager &vulkan_manager, InputFormat input_format, uint32_t input_block_size,
                  bool benchmark_shaders)
            : m_executable_dir(std::move(executable_dir)),
              m_filename(std::move(filename)),
              m_sample_frequency(sample_frequency),
              m_input_format(input_format),
              m_input_block_size(input_block_size),
              m_vulkan_manager(vulkan_manager),
              m_benchmark_shaders(benchmark_shaders),
              m_log(log),
              m_input_reader(nullptr),
              m_input_is_fifo(false),
              m_total_samples_read(0),
              m_vacant_blocks(),
              m_filled_blocks(),
              m_demodulated_block_mutex(),
              m_cv_filled(),
              m_cv_vacant(),
              m_stop_request(false),
              m_reader_thread_finished(false) {
    }

    RfDemodulator(const RfDemodulator&) = delete;
    void operator=(const RfDemodulator&) = delete;

    // Derived classes call cleanup() from their own destructors, so the
    // demodulator thread is joined while the demodulate() override it is
    // running still exists.  The call here is the idempotent backstop.
    ~RfDemodulator() {
        cleanup();
    }

    bool initialize(int number_of_block_buffers) {
        m_input_reader = makeInputReader(m_filename, m_input_format, m_input_block_size, &m_log);
        m_input_reader->setDcBlocking(true); // RF carries no legitimate DC
        m_input_is_fifo = m_input_reader->isLive();
        m_input_reader->initialize();

        for (int i = 0; i < number_of_block_buffers; i++)
            m_vacant_blocks.push_back(std::make_unique<B>(m_vulkan_manager));

        m_demodulator_thread = std::thread([this]() {
#ifdef __APPLE__
            pthread_setname_np("museld-demod");
#elif defined(linux) || defined(__FreeBSD__)
            pthread_setname_np(pthread_self(), "museld-demod");
#endif
            // Nothing above this catches: an exception escaping the thread would
            // terminate the process.  Store it and signal end of stream instead;
            // getNextDemodulatedBlock() rethrows it on the reader thread, whose
            // own marshalling carries it on to the main thread.
            try {
                demodulate();
            } catch (...) {
                std::unique_lock<std::mutex> lock(m_demodulated_block_mutex);
                m_demod_exception = std::current_exception();
                m_reader_thread_finished = true;
                m_cv_filled.notify_all();
            }
        });
        return true;
    }

    std::unique_ptr<B> getNextDemodulatedBlock() {
        std::unique_lock<std::mutex> lock(m_demodulated_block_mutex);
        // m_stop_request belongs in the predicate, not just in a notify: waking a
        // wait only asks the question again, so without it here a waiter would
        // find no blocks and an unfinished thread and go back to sleep, and
        // requestStop()'s notify would do nothing.  The frame readers rely on
        // this: their cleanup() calls requestStop() precisely to unblock a
        // reader thread waiting here before joining it.
        m_cv_filled.wait(
                lock,
                [this] { return m_stop_request || m_reader_thread_finished || !m_filled_blocks.empty(); });
        if (m_stop_request || m_filled_blocks.empty()) {
            // Only report a demodulator error when the stream ended on its own;
            // a requested stop is a shutdown, where the error no longer matters.
            if (!m_stop_request && m_demod_exception)
                std::rethrow_exception(std::exchange(m_demod_exception, nullptr));
            return nullptr;
        }

        auto block = std::move(m_filled_blocks.front());
        m_filled_blocks.pop_front();
        return block;
    }

    void returnBlock(std::unique_ptr<B> &buffer) {
        std::unique_lock<std::mutex> lock(m_demodulated_block_mutex);
        m_cv_vacant.notify_one();
        m_vacant_blocks.push_back(std::move(buffer));
    }

    // Seeks.  Both return the new seek generation: every block whose input
    // is read after the seek carries it, so a consumer can tell the first
    // block after the seek from the ones still in flight before it.  The
    // input position and its bookkeeping are updated under m_input_mutex,
    // which readInput() holds while it reads a block, so the relative seek
    // is relative to the block boundary the demodulator thread is at, and
    // the absolute one lands where it says (to the input reader's own
    // granularity).  A seek the input refuses (nullopt) changes nothing:
    // the demodulator carries on where it was.  Live input cannot seek; the
    // generation is returned unchanged.
    std::optional<uint32_t> seek(double seconds) {
        if (m_input_is_fifo)
            return m_seek_generation;
        std::unique_lock<std::mutex> input_lock(m_input_mutex);
        int64_t samples_to_seek = (int64_t)(seconds * m_sample_frequency);
        m_log.info(eInput, std::format("Seeking relative time {} s, {} samples.",
                                       seconds, samples_to_seek));
        return seekLocked(samples_to_seek);
    }

    std::optional<uint32_t> seekToSample(int64_t sample) {
        if (m_input_is_fifo)
            return m_seek_generation;
        std::unique_lock<std::mutex> input_lock(m_input_mutex);
        const int64_t samples_to_seek = std::max<int64_t>(0, sample) - m_total_samples_read;
        m_log.debug(eInput, std::format("Seeking to sample {} ({:+} from the current position)",
                                        sample, samples_to_seek));
        return seekLocked(samples_to_seek);
    }

    [[nodiscard]] int64_t inputSampleCount() const {
        return m_input_reader ? m_input_reader->sampleCount() : -1;
    }

    // Wake everything waiting on this demodulator without joining anything, so
    // a caller can unblock its own consumer thread before joining it.
    void requestStop() {
        // Set the flag before notifying, as the EFM worker's stop does: the
        // waiters test it, so a notify that precedes it wakes them to find
        // nothing changed.
        std::unique_lock<std::mutex> lock(m_demodulated_block_mutex);
        m_stop_request = true;
        m_cv_vacant.notify_all();
        m_cv_filled.notify_all();
    }

    // Idempotent: runs both from the explicit teardown path and from the destructors.
    void cleanup() {
        requestStop();
        m_log.debug(eInput, "RfDemodulator: requested stop");
        if (m_demodulator_thread.joinable())
            m_demodulator_thread.join();
        m_input_reader.reset();
        m_vacant_blocks.clear();
        m_filled_blocks.clear();
    }

protected:
    virtual void demodulate() = 0;

    // Reads one input block for the demodulator thread.  On return
    // block_input_offset is the input sample position the block starts at
    // and seek_generation the seek count its data belongs to; both are
    // taken under the same lock as the read itself, so a seek from another
    // thread lands either wholly before or wholly after this block.
    bool readInput(float *out, size_t n, int64_t &block_input_offset, uint32_t &seek_generation) {
        assert(n == m_input_block_size);
        if (m_stop_request) {
            m_log.info(eInput, "RfDemodulator: stop requested");
            return false;
        }
        std::unique_lock<std::mutex> input_lock(m_input_mutex);
        int got = m_input_reader->readFloats(out);
        if (got == 0) {
            m_log.info(eInput, "RfDemodulator: end of file");
            return false;
        }
        block_input_offset = m_total_samples_read;
        seek_generation = m_seek_generation;
        m_total_samples_read += (int64_t)n;
        return true;
    }

    // m_input_mutex held by the caller
    std::optional<uint32_t> seekLocked(int64_t samples_to_seek) {
        if (!m_input_reader->seek(samples_to_seek)) {
            m_log.warn(eInput, "The input refused the seek; playback continues where it was");
            return std::nullopt;
        }
        m_total_samples_read = std::max<int64_t>(0, m_total_samples_read + samples_to_seek);
        const uint32_t generation = ++m_seek_generation;

        // Discard any filled buffers
        std::unique_lock<std::mutex> lock(m_demodulated_block_mutex);
        for (auto &b : m_filled_blocks)
            m_vacant_blocks.push_back(std::move(b));
        m_filled_blocks.clear();
        m_cv_vacant.notify_one();
        return generation;
    }

    const std::string m_executable_dir;
    const std::string m_filename;
    const float m_sample_frequency;
    const InputFormat m_input_format;
    const uint32_t m_input_block_size;
    musevk::VulkanManager &m_vulkan_manager;
    bool m_benchmark_shaders;
    Logger &m_log;
    std::unique_ptr<InputReader> m_input_reader;
    bool m_input_is_fifo;
    std::mutex m_input_mutex; // the input reader's position and the two fields below
    int64_t m_total_samples_read;
    uint32_t m_seek_generation = 0;
    std::deque<std::unique_ptr<B>> m_vacant_blocks;
    std::deque<std::unique_ptr<B>> m_filled_blocks;
    std::mutex m_demodulated_block_mutex; // used to synchronize access to the vacant / filled blocks
    std::condition_variable m_cv_filled;
    std::condition_variable m_cv_vacant;
    std::atomic<bool> m_stop_request;
    std::atomic<bool> m_reader_thread_finished;
    std::exception_ptr m_demod_exception; // set by the demodulator thread; guarded by m_demodulated_block_mutex
    std::thread m_demodulator_thread; // last member: joined before the state above goes away
};

#endif //MUSECPP_RFDEMODULATOR_H
