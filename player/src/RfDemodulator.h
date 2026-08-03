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
              m_demodulator_thread(nullptr),
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

    bool initialize(int number_of_block_buffers) {
        m_input_reader = makeInputReader(m_filename, m_input_format, m_input_block_size);
        m_input_reader->setDcBlocking(true); // RF carries no legitimate DC
        m_input_is_fifo = m_input_reader->is_fifo();
        m_input_reader->initialize();

        for (int i = 0; i < number_of_block_buffers; i++)
            m_vacant_blocks.push_back(std::make_unique<B>(m_vulkan_manager));

        m_demodulator_thread = new std::thread([this]() {
#ifdef __APPLE__
            pthread_setname_np("museld-demod");
#elif defined(linux) || defined(__FreeBSD__)
            pthread_setname_np(pthread_self(), "museld-demod");
#endif
            // Nothing above this catches: an exception escaping the thread would otherwise
            // terminate the process, burying the reason in the runtime's abort message.
            try {
                demodulate();
            } catch (const std::exception &x) {
                m_log.error(eInput, x.what());
                std::exit(EXIT_FAILURE);
            }
        });
        return true;
    }

    std::unique_ptr<B> getNextDemodulatedBlock() {
        std::unique_lock<std::mutex> lock(m_demodulated_block_mutex);
        // m_stop_request belongs in the predicate, not just in a notify: waking a
        // wait only asks the question again, so without it here a waiter would
        // find no blocks and an unfinished thread and go back to sleep, and
        // cleanup()'s notify below would do nothing. Nobody waits here while
        // cleanup() runs today -- the reader thread is joined before the
        // demodulator is stopped -- but that is an argument about ordering
        // elsewhere, not something this function should depend on.
        m_cv_filled.wait(
                lock,
                [this] { return m_stop_request || m_reader_thread_finished || !m_filled_blocks.empty(); });
        if (m_stop_request || m_filled_blocks.empty())
            return nullptr;

        auto block = std::move(m_filled_blocks.front());
        m_filled_blocks.pop_front();
        return block;
    }

    void returnBlock(std::unique_ptr<B> &buffer) {
        std::unique_lock<std::mutex> lock(m_demodulated_block_mutex);
        m_cv_vacant.notify_one();
        m_vacant_blocks.push_back(std::move(buffer));
    }

    void seek(double seconds) {
        if (m_input_is_fifo)
            return;
        long samples_to_seek = (long)(seconds * m_sample_frequency);
        m_log.info(eInput, std::format("Seeking relative time {} s, {} samples.",
                                       seconds, samples_to_seek));
        m_input_reader->seek(samples_to_seek);
        m_total_samples_read = std::max(0L, m_total_samples_read + samples_to_seek);

        // Discard any filled buffers
        std::unique_lock<std::mutex> lock(m_demodulated_block_mutex);
        for (auto &b : m_filled_blocks)
            m_vacant_blocks.push_back(std::move(b));
        m_filled_blocks.clear();
        m_cv_vacant.notify_one();
    }

    void cleanup() {
        {
            // Set the flag before notifying, as the EFM worker's stop does: the
            // waiters test it, so a notify that precedes it wakes them to find
            // nothing changed.
            std::unique_lock<std::mutex> lock(m_demodulated_block_mutex);
            m_stop_request = true;
            m_cv_vacant.notify_all();
            m_cv_filled.notify_all();
        }
        m_log.debug(eInput, "RfDemodulator: requested stop");
        m_demodulator_thread->join();
        delete m_demodulator_thread;
        m_input_reader.reset();
        m_vacant_blocks.clear();
        m_filled_blocks.clear();
    }

protected:
    virtual void demodulate() = 0;

    // fir_filter.comp and input_fir_filter.comp size their shared memory from the tap count and
    // decimation they were specialized to, so how much they ask for is only known once they have
    // been created -- and nothing outside the validation layer objects if it is more than the
    // device has.  They also load the tap array with one invocation per tap, so a filter longer
    // than the workgroup would be silently truncated.  Check both here, against what this device
    // reports rather than the 16 KiB that maxComputeSharedMemorySize is merely guaranteed to be.
    void checkFirShaderFits(std::string_view shader_name, const musevk::ComputeShader &shader,
                            uint32_t max_filter_size, uint32_t max_decimation) const {
        const uint32_t local_size = shader.getLocalWorkgroupSize().x_size;

        if (max_filter_size > local_size)
            throw std::runtime_error(std::format(
                    "{} was given a {}-tap filter, more than the {} invocations in its workgroup, "
                    "which load the taps one each. Filter lengths follow --sample-freq.",
                    shader_name, max_filter_size, local_size));

        const uint32_t needed = (2 * max_filter_size + local_size * max_decimation) * sizeof(float);
        const uint32_t available = m_vulkan_manager.getPhysicalDeviceProperties().limits.maxComputeSharedMemorySize;
        m_log.debug(eVideo, std::format(
                "{}: {} taps, decimation {}, workgroup {} -> {} bytes of shared memory (device offers {})",
                shader_name, max_filter_size, max_decimation, local_size, needed, available));
        if (needed > available)
            throw std::runtime_error(std::format(
                    "Unsupported hardware: {} needs {} bytes of shared memory for a {}-tap filter "
                    "decimating by {} at a workgroup of {}, but this device offers only {} "
                    "(maxComputeSharedMemorySize).",
                    shader_name, needed, max_filter_size, max_decimation, local_size, available));
    }

    bool readFloats(float *out, size_t n) {
        assert(n == m_input_block_size);
        if (m_stop_request) {
            m_log.info(eInput, "RfDemodulator: stop requested");
            return false;
        }
        int got = m_input_reader->readFloats(out);
        if (got == 0) {
            m_log.info(eInput, "RfDemodulator: end of file");
            return false;
        }
        return true;
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
    long m_total_samples_read;
    std::thread *m_demodulator_thread;
    std::deque<std::unique_ptr<B>> m_vacant_blocks;
    std::deque<std::unique_ptr<B>> m_filled_blocks;
    std::mutex m_demodulated_block_mutex; // used to synchronize access to the vacant / filled blocks
    std::condition_variable m_cv_filled;
    std::condition_variable m_cv_vacant;
    std::atomic<bool> m_stop_request;
    std::atomic<bool> m_reader_thread_finished;
};

#endif //MUSECPP_RFDEMODULATOR_H
