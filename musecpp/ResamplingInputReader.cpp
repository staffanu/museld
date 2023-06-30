//
// Created by staffanu on 6/30/23.
//

#include <fcntl.h>
#include <csignal>
#include "MuseTypes.h"
#include "ResamplingInputReader.h"

using namespace std;

ResamplingInputReader::ResamplingInputReader(
        const std::string &filename, int sample_rate, bool stop_on_eof)
        : InputReader(filename, stop_on_eof),
          m_input_pll(sample_rate),
          m_file_fd(-1),
          m_file_input_buffer{},
          m_t(0),
          m_file_input_buffer_bytes(c_input_buffer_min_read_pos),
          m_file_input_buffer_read_pos(c_input_buffer_min_read_pos) {
}

bool ResamplingInputReader::initialize(std::vector<std::shared_ptr<musevk::VulkanBuffer>> const &buffers) {
    m_file_fd = open(m_filename.c_str(), 0);
    if (m_file_fd == -1)
        throw runtime_error("Unable to open file");

    cout << "Seeking: " << lseek(m_file_fd, 500000 * 1000, SEEK_SET) << endl;

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
        int bytes_to_read = (int)t;
        t -= bytes_to_read;

        if (read_pos + bytes_to_read >= m_file_input_buffer_bytes) {
            // move remaining input to start of buffer
            int start_copy_pos = read_pos - c_input_buffer_min_read_pos;
            int n = m_file_input_buffer_bytes - start_copy_pos;
            if (n > 0) // 0 first time, and could be later if we have short reads
                memcpy(m_file_input_buffer, m_file_input_buffer + start_copy_pos, n);
            m_file_input_buffer_bytes -= start_copy_pos;
            read_pos = c_input_buffer_min_read_pos;

            do {
                ssize_t read_count = read(m_file_fd,
                                          (void *) (m_file_input_buffer + m_file_input_buffer_bytes),
                                          c_input_buffer_size - m_file_input_buffer_bytes);
                if (read_count == -1)
                    throw runtime_error("Error reading from file");
                else if (read_count == 0 && m_stop_on_eof) {
                    return false;
                }
                m_file_input_buffer_bytes += (int)read_count;
            } while (read_pos + bytes_to_read >= m_file_input_buffer_bytes);
        }
        read_pos += bytes_to_read;

        double b0 = m_file_input_buffer[read_pos - 3];
        double b1 = m_file_input_buffer[read_pos - 2];
        double b2 = m_file_input_buffer[read_pos - 1];
        double b3 = m_file_input_buffer[read_pos - 0];

        double x = 1 + t;
        // cubic spline though 4 points, f(x) = a0 + a1 x + a2 x(x-1) + a3 x(x-1)(x-2)
        double a0 = b0;
        double a1 = b1 - a0;
        double a2 = (b2 - a0 - 2 * a1) / 2;
        double a3 = (b3 - a0 - 3 * a1 - 6 * a2) / 6;
        // now evaluate at point 1 + p
        double y = a0 + a1 * x + a2 * x * (x - 1) + a3 * x * (x - 1) * (x - 2);

        buffer[sample_ix] = (uint16_t)(y * MUSE_SHORT_INPUT_MULT);
    }
    m_t = t;
    m_file_input_buffer_read_pos = read_pos;
    return true;
}
