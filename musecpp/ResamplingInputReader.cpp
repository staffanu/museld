//
// Created by staffanu on 6/30/23.
//

#include <fcntl.h>
#include <unistd.h>
#include <fmt/format.h>
#include "MuseTypes.h"
#include "ResamplingInputReader.h"

using namespace std;

ResamplingInputReader::ResamplingInputReader(
        Logger &log,
        const std::string &filename, InputFormat input_format, double sample_rate,
        bool input_is_fifo, double initial_seek_seconds,
        const std::optional<std::string> &output_filename)
        : InputReader(log, filename, input_is_fifo, initial_seek_seconds, output_filename),
          m_input_format(input_format),
          m_input_pll(log, sample_rate),
          m_input_is_fifo(input_is_fifo),
          m_file_fd(-1),
          m_file_input_buffer{},
          m_t(0),
          m_file_input_buffer_bytes(c_input_buffer_lookback),
          m_file_input_buffer_read_pos(c_input_buffer_lookback) {
    switch (m_input_format) {
        case eUnsignedByte:
            m_bytes_per_sample = 1;
            m_output_multiplier = MUSE_SHORT_INPUT_MULT;
            m_output_add = 0;
            break;
        case eSignedShortLittleEndian:
            m_bytes_per_sample = 2;
            m_output_multiplier = 1024.0 / 65536.0;
            m_output_add = 512;
            break;
        default:
            throw runtime_error("Unrecognized input format");
    }
}

bool ResamplingInputReader::initialize(std::vector<std::shared_ptr<musevk::VulkanBuffer>> const &buffers) {
    m_file_fd = open(m_filename.c_str(), O_NONBLOCK);
    if (m_file_fd == -1)
        throw runtime_error("Unable to open file");

#ifdef linux
    if (m_input_is_fifo) {
        m_log.debug(eInput, fmt::format("Pipe size: {}", fcntl(m_file_fd, F_GETPIPE_SZ)));
        fcntl(m_file_fd, F_SETPIPE_SZ, 1024 * 1024);
        m_log.debug(eInput, fmt::format("Pipe size now: {}", fcntl(m_file_fd, F_GETPIPE_SZ)));
    }
#endif
    if (m_initial_seek_seconds != 0) {
        size_t samples_to_seek = (size_t)(m_initial_seek_seconds * 16.2e6 * m_input_pll.getInputSamplesPerSample());
        size_t bytes_to_seek = m_bytes_per_sample * samples_to_seek;
        m_log.info(eInput, fmt::format("Seeking to time {} s, {} samples, {} bytes.",
                                       m_initial_seek_seconds, samples_to_seek, bytes_to_seek));
        lseek(m_file_fd, bytes_to_seek, SEEK_SET);
    }

    return InputReader::initialize(buffers);
}

void ResamplingInputReader::cleanup() {
    InputReader::cleanup();

    if (m_file_fd != -1)
        close(m_file_fd);
}

void ResamplingInputReader::threadFunc() {
    //pthread_setname_np(m_reader_thread->native_handle(), "musecpp-reader");

    uint16_t sample_buffer[c_sample_buffer_size];
    double input_samples_per_sample = m_input_pll.getInputSamplesPerSample();
    int samples_to_read = 1;
    shared_ptr<musevk::VulkanBuffer> buffer = nullptr;
    while (readSamples(samples_to_read, sample_buffer, input_samples_per_sample)) {
        if (buffer == nullptr) {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_input_is_fifo && m_vacant_muse_input_buffers.empty()) {
                // discard a filled buffer -- this is better than having the writer to the fifo wait
                m_log.warn(eInput, "Discarding filled input buffer due to overrun");
                assert(!m_filled_muse_input_buffers.empty());
                m_vacant_muse_input_buffers.push_back(m_filled_muse_input_buffers.back());
                m_filled_muse_input_buffers.pop_back();
            }
            m_cv_vacant.wait(lock, [this]{return m_stop_request || !m_vacant_muse_input_buffers.empty();});
            if (m_stop_request)
                break;
            buffer = m_vacant_muse_input_buffers.front();
            m_vacant_muse_input_buffers.pop_front();
        }

        auto data = buffer->data<uint16_t>();
        InputPll::PllResult pll_result = m_input_pll.process(samples_to_read, sample_buffer, data);
        samples_to_read = pll_result.samples_to_read;
        input_samples_per_sample = pll_result.input_samples_per_sample;

        if (pll_result.frame_done) {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv_filled.notify_one();
            m_filled_muse_input_buffers.push_back(buffer);
            buffer = nullptr;
        }
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv_filled.notify_one();
    m_reader_thread_finished = true;
}

bool ResamplingInputReader::readSamples(int sample_count, uint16_t buffer[c_sample_buffer_size], double dt) {
    double t = m_t; // always in [0, 1)
    int read_pos = m_file_input_buffer_read_pos;
    for (int sample_ix = 0; sample_ix < sample_count; sample_ix++) {
        t += dt;
        int samples_to_read = (int)t;
        t -= samples_to_read;

        if (read_pos + samples_to_read * m_bytes_per_sample >= m_file_input_buffer_bytes) {
            // move remaining input to start of buffer
            int start_copy_pos = read_pos - c_input_buffer_lookback * m_bytes_per_sample;
            int n = m_file_input_buffer_bytes - start_copy_pos;
            if (n > 0) // 0 first time, and could be later if we have short reads
                memcpy(m_file_input_buffer, m_file_input_buffer + start_copy_pos, n);
            m_file_input_buffer_bytes -= start_copy_pos;
            read_pos = c_input_buffer_lookback * m_bytes_per_sample;

            do {
                ssize_t read_count = read(m_file_fd,
                                          (void *) (m_file_input_buffer + m_file_input_buffer_bytes),
                                          c_input_buffer_size - m_file_input_buffer_bytes);
                if (read_count == -1 && errno == EAGAIN)
                    this_thread::sleep_for(chrono::milliseconds(1));
                else if (read_count == 0) {
                    if (!m_input_is_fifo)
                        return false;
                } else if (read_count == -1)
                    throw runtime_error(fmt::format("Error reading from file: {}", strerror(errno)));
                else
                    m_file_input_buffer_bytes += (int)read_count;
            } while (read_pos + samples_to_read * m_bytes_per_sample >= m_file_input_buffer_bytes);
        }
        read_pos += samples_to_read * m_bytes_per_sample;

        double b0, b1, b2, b3;
        switch (m_input_format) {
            case eUnsignedByte:
                b0 = m_file_input_buffer[read_pos - 3];
                b1 = m_file_input_buffer[read_pos - 2];
                b2 = m_file_input_buffer[read_pos - 1];
                b3 = m_file_input_buffer[read_pos - 0];
                break;
            case eSignedShortLittleEndian:
                b0 = ((int16_t *)(m_file_input_buffer + read_pos))[-3];
                b1 = ((int16_t *)(m_file_input_buffer + read_pos))[-2];
                b2 = ((int16_t *)(m_file_input_buffer + read_pos))[-1];
                b3 = ((int16_t *)(m_file_input_buffer + read_pos))[0];
                break;
            default:
                throw runtime_error("Unrecognized input format");
        }

        double x = 1 + t;
        // cubic spline though 4 points, f(x) = a0 + a1 x + a2 x(x-1) + a3 x(x-1)(x-2)
        double a0 = b0;
        double a1 = b1 - a0;
        double a2 = (b2 - a0 - 2 * a1) / 2;
        double a3 = (b3 - a0 - 3 * a1 - 6 * a2) / 6;
        // now evaluate at point 1 + p
        double y = a0 + a1 * x + a2 * x * (x - 1) + a3 * x * (x - 1) * (x - 2);

        auto muse_int = (uint16_t)(y * m_output_multiplier + m_output_add);
        buffer[sample_ix] = muse_int;
    }
    m_t = t;
    m_file_input_buffer_read_pos = read_pos;
    return true;
}
