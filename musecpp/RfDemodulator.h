//
// Created by staffanu on 12/10/23.
//

#ifndef MUSECPP_RFDEMODULATOR_H
#define MUSECPP_RFDEMODULATOR_H

#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <fmt/format.h>
#include <cassert>
#include <complex>
#include <array>
#include <filesystem>
#include <algorithm>
#include <thread>
#include <atomic>
#include <deque>
#include <condition_variable>
#include "util/Logger.h"
#include "musevk/VulkanManager.h"

template<class B>
class RfDemodulator {
public:
    RfDemodulator(Logger &log, std::string executable_dir, std::string filename,
                  musevk::VulkanManager &vulkan_manager, bool benchmark_shaders, float sample_frequency)
            : c_sample_frequency(sample_frequency),
              m_log(log),
              m_executable_dir(std::move(executable_dir)),
              m_filename(std::move(filename)),
              m_vulkan_manager(vulkan_manager),
              m_benchmark_shaders(benchmark_shaders),
              m_input_fd(-1),
              m_input_is_fifo(std::filesystem::is_fifo(filename)),
              m_total_samples_read(0),
              m_demodulator_thread(nullptr),
              m_vacant_blocks(),
              m_filled_blocks(),
              m_demodulated_block_mutex(),
              m_input_file_mutex(),
              m_cv_filled(),
              m_cv_vacant(),
              m_stop_request(false),
              m_reader_thread_finished(false) {
    }

    RfDemodulator(const RfDemodulator&) = delete;
    void operator=(const RfDemodulator&) = delete;

    bool initialize(int number_of_block_buffers) {
        m_input_fd = open(m_filename.c_str(), O_NONBLOCK);
        if (m_input_fd == -1)
            throw std::runtime_error(fmt::format("RfDemodulator: Unable to open input file {}", m_filename));

#ifdef linux
        if (m_input_is_fifo) {
            m_log.debug(eInput, fmt::format("Pipe size: {}", fcntl(m_input_fd, F_GETPIPE_SZ)));
            fcntl(m_input_fd, F_SETPIPE_SZ, 1024 * 1024);
            m_log.debug(eInput, fmt::format("Pipe size now: {}", fcntl(m_input_fd, F_GETPIPE_SZ)));
        }
#endif

        for (int i = 0; i < number_of_block_buffers; i++)
            m_vacant_blocks.push_back(std::make_unique<B>(m_vulkan_manager));

        m_demodulator_thread = new std::thread(&RfDemodulator<B>::demodulate, this);
#ifdef linux
        pthread_setname_np(m_demodulator_thread->native_handle(), "musecpp-demod");
#endif

        return true;
    }

    std::unique_ptr<B> getNextDemodulatedBlock() {
        std::unique_lock<std::mutex> lock(m_demodulated_block_mutex);
        m_cv_filled.wait(
                lock,
                [this] { return m_reader_thread_finished || !m_filled_blocks.empty(); });
        if (m_filled_blocks.empty())
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
        if (!m_input_is_fifo) {
            std::unique_lock<std::mutex> lock(m_input_file_mutex);

            long samples_to_seek = (long)(seconds * c_sample_frequency);
            off_t bytes_to_seek = 2 * samples_to_seek;
            m_log.info(eInput, fmt::format("Seeking relative time {} s, {} samples, {} bytes.",
                                           seconds, samples_to_seek, bytes_to_seek));
            lseek(m_input_fd, bytes_to_seek, SEEK_CUR);
            m_total_samples_read = std::max(0L, m_total_samples_read + samples_to_seek); // FIXME: locking?

            // Discard any filled buffers
            std::unique_lock<std::mutex> lock2(m_demodulated_block_mutex);
            for (auto &b : m_filled_blocks)
                m_vacant_blocks.push_back(std::move(b));
            m_filled_blocks.clear();
            m_cv_vacant.notify_one();
        }
    }

    void cleanup() {
        {
            std::unique_lock<std::mutex> lock(m_demodulated_block_mutex);
            m_cv_vacant.notify_one();
            m_cv_filled.notify_one();
            m_stop_request = true;
        }
        m_log.debug(eInput, "RfDemodulator: requested stop");
        m_demodulator_thread->join();
        delete m_demodulator_thread;
        close(m_input_fd);
        m_vacant_blocks.clear();
        m_filled_blocks.clear();
    }

protected:
    virtual void demodulate() = 0;

    bool readFloats(int16_t *out, size_t n) {
        int filled_bytes = 0;
        do {
            if (m_stop_request) {
                m_log.info(eInput, "RfDemodulator: stop requested");
                return false;
            }
            std::scoped_lock<std::mutex> lock(m_input_file_mutex);
            ssize_t read_count = read(m_input_fd,
                                      (void *)((char *)out + filled_bytes),
                                      n * sizeof(int16_t) - filled_bytes);
            if (read_count == -1 && errno == EAGAIN)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            else if (read_count == 0) {
                if (!m_input_is_fifo) {
                    m_log.info(eInput, "RfDemodulator: end of file");
                    return false;
                }
            } else if (read_count == -1)
                throw std::runtime_error(fmt::format("Error reading from file: {}", strerror(errno)));
            else
                filled_bytes += (int)read_count;
        } while (filled_bytes < n * sizeof(int16_t));

        return true;
    }

    const float c_sample_frequency;
    Logger &m_log;
    const std::string m_executable_dir;
    const std::string m_filename;
    musevk::VulkanManager &m_vulkan_manager;
    bool m_benchmark_shaders;
    int m_input_fd;
    bool m_input_is_fifo;
    long m_total_samples_read;
    std::thread *m_demodulator_thread;
    std::deque<std::unique_ptr<B>> m_vacant_blocks;
    std::deque<std::unique_ptr<B>> m_filled_blocks;
    std::mutex m_demodulated_block_mutex; // used to synchronize access to the vacant / filled blocks
    std::mutex m_input_file_mutex; // used to make sure we do not seek during a file read
    std::condition_variable m_cv_filled;
    std::condition_variable m_cv_vacant;
    std::atomic<bool> m_stop_request;
    std::atomic<bool> m_reader_thread_finished;
};

#endif //MUSECPP_RFDEMODULATOR_H
