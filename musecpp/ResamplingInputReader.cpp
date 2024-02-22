//
// Created by staffanu on 6/30/23.
//

#include <fcntl.h>
#include <unistd.h>
#include <fmt/format.h>
#include <cassert>
#include <filesystem>
#include "musevk/VulkanBuffer.h"
#include "ResamplingInputReader.h"
#include "util/Logger.h"

using namespace std;

ResamplingInputReader::ResamplingInputReader(
        Logger &log,
        const std::string &filename, InputFormat input_format, double sample_rate,
        double initial_seek_seconds, bool demodulate,
        const std::optional<std::string> &output_filename)
        : InputReader(log, filename,
                      filesystem::is_fifo(filename),
                      initial_seek_seconds, output_filename),
          m_input_format(input_format),
          m_sample_rate(sample_rate),
          m_input_pll(log),
          m_efm_pll(log),
          m_file_fd(-1),
          m_demodulator(nullptr),
          m_file_input_buffer{},
          m_file_input_buffer_bytes(c_input_buffer_lookback * 4), // 4 >= input bytes per sample
          m_file_input_buffer_read_pos(c_input_buffer_lookback * 4),
          m_dropout_input_buffer{},
          m_t(0) {
    if (demodulate) {
        m_input_format = eFloat;
        m_demodulator = new RfDemodulator(log, m_filename);
        m_sample_rate = 31.25e6;
    }

    switch (m_input_format) {
        case eUnsignedByte:
            m_bytes_per_sample = 1;
            m_output_multiplier = 1;
            m_output_add = 0;
            break;
        case eSignedShortLittleEndian:
            m_bytes_per_sample = 2;
            m_output_multiplier = 1.0 / 256.0;
            m_output_add = 128;
            break;
        case eFloat:
            m_bytes_per_sample = 4;
            m_output_multiplier = 1;
            m_output_add = 0;
            break;
        default:
            throw runtime_error("Unrecognized input format");
    }
}

bool ResamplingInputReader::initialize(std::vector<std::shared_ptr<InputReader::InputReaderBlock>> &buffers) {
    if (m_demodulator == nullptr) {
        m_file_fd = open(m_filename.c_str(), O_NONBLOCK);
        if (m_file_fd == -1)
            throw runtime_error(fmt::format("ResamplingInputReader: Unable to open input file {}", m_filename));
        memset(m_dropout_input_buffer, 0, sizeof(m_dropout_input_buffer));
#ifdef linux
        if (filesystem::is_fifo(m_filename)) {
            m_log.debug(eInput, fmt::format("Pipe size: {}", fcntl(m_file_fd, F_GETPIPE_SZ)));
            fcntl(m_file_fd, F_SETPIPE_SZ, 1024 * 1024);
            m_log.debug(eInput, fmt::format("Pipe size now: {}", fcntl(m_file_fd, F_GETPIPE_SZ)));
        }
#endif
    } else {
        m_demodulator->initialize();
    }

    m_input_pll.initialize(m_sample_rate);

    return InputReader::initialize(buffers);
}

void ResamplingInputReader::cleanup() {
    InputReader::cleanup();

    if (m_demodulator != nullptr) {
        m_demodulator->cleanup();
        delete m_demodulator;
    }

    if (m_file_fd != -1)
        close(m_file_fd);
}

void ResamplingInputReader::seek(double seconds) {
    if (!m_input_is_realtime) {
        if (m_demodulator != nullptr) {
            m_demodulator->seek(seconds);
        } else {
            std::unique_lock<std::mutex> lock(m_mutex);

            off_t samples_to_seek = (off_t) (seconds * 16.2e6 * m_input_pll.getInputSamplesPerSample());
            off_t bytes_to_seek = m_bytes_per_sample * samples_to_seek;
            m_log.info(eInput, fmt::format("Seeking relative time {} s, {} samples, {} bytes.",
                                           seconds, samples_to_seek, bytes_to_seek));
            lseek(m_file_fd, bytes_to_seek, SEEK_CUR);

            // discard content in existing input buffers
            copy(m_filled_muse_input_buffers.begin(), m_filled_muse_input_buffers.end(),
                 back_inserter(m_vacant_muse_input_buffers));
            m_filled_muse_input_buffers.clear();
            m_cv_vacant.notify_one();
        }
        m_input_pll.setUnlocked(); // do not wait to discover that we lost sync
    }
}

void ResamplingInputReader::threadFunc() {
    float sample_buffer[c_sample_buffer_size];
    uint8_t dropout_buffer[c_sample_buffer_size];
    float efm_sample_buffer[DemodulatedBlock::c_efm_block_size];
    bool have_efm;
    double input_samples_per_sample = m_input_pll.getInputSamplesPerSample();
    int samples_to_read = 1;
    shared_ptr<InputReaderBlock> buffer = nullptr;
    while (readSamples(samples_to_read, sample_buffer, dropout_buffer, input_samples_per_sample, efm_sample_buffer, have_efm)) {
        if (buffer == nullptr) {
            unique_lock<std::mutex> lock(m_mutex);
            if (m_input_is_realtime && m_vacant_muse_input_buffers.empty()) {
                // discard a filled buffer -- this is better than having the writer to the fifo wait
                m_log.warn(eInput, "Discarding filled input buffer due to overrun");
                assert(!m_filled_muse_input_buffers.empty());
                m_vacant_muse_input_buffers.push_back(m_filled_muse_input_buffers.back());
                m_filled_muse_input_buffers.pop_back();
            }
            m_cv_vacant.wait(lock, [this]{return m_stop_request || !m_vacant_muse_input_buffers.empty();});
            if (m_stop_request) {
                m_log.info(eInput, "ResamplingInputReader: stop requested");
                break;
            }
            buffer = m_vacant_muse_input_buffers.front();
            m_vacant_muse_input_buffers.pop_front();
            buffer->efm_data_size = 0;;
        }

        auto video_data = buffer->video_data->data<float>();
        auto dropout_data = buffer->dropout_data->data<uint8_t>();
        InputPll::PllResult pll_result = m_input_pll.process(samples_to_read, sample_buffer, dropout_buffer, video_data, dropout_data);
        samples_to_read = pll_result.samples_to_read;
        input_samples_per_sample = pll_result.input_samples_per_sample;

        if (pll_result.locked) {
            if (have_efm) {
                bool efm_bit_buffer[DemodulatedBlock::c_efm_block_size]; // much bigger than needed
                int actual_output_size = m_efm_pll.reclock(efm_sample_buffer, DemodulatedBlock::c_efm_block_size,
                                                           efm_bit_buffer, DemodulatedBlock::c_efm_block_size);

                for (int i = 0; i < actual_output_size; i++)
                    buffer->efm_data[buffer->efm_data_size++] = efm_bit_buffer[i];
                assert(buffer->efm_data_size <= buffer->c_max_efm_data_size);
            }
        } else
            buffer->efm_data_size = 0;;

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

// TODO: This interface isn't very good when reading constant size blocks from the demodulator -- rewrite somehow!
// This is especially apparent in how EFM is handled.
bool ResamplingInputReader::readSamples(int sample_count, float buffer[c_sample_buffer_size],
                                        uint8_t dropout_buffer[c_sample_buffer_size], double dt,
                                        float efm_buffer[DemodulatedBlock::c_efm_block_size], bool &have_efm) {
    have_efm = false;
    double t = m_t; // always in [0, 1)
    int read_pos = m_file_input_buffer_read_pos;
    for (int sample_ix = 0; sample_ix < sample_count; sample_ix++) {
        t += dt;
        int samples_to_read = (int)t;
        t -= samples_to_read;

        double b0, b1, b2, b3;
        if (m_demodulator == nullptr) {
            if (read_pos + samples_to_read * m_bytes_per_sample >= m_file_input_buffer_bytes) {
                // move remaining input to start of buffer
                int start_copy_pos = read_pos - c_input_buffer_lookback * m_bytes_per_sample;
                int n = m_file_input_buffer_bytes - start_copy_pos;
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
                        if (!m_input_is_realtime) {
                            m_log.info(eInput, "ResamplingInputReader: end of file");
                            return false;
                        }
                    } else if (read_count == -1)
                        throw runtime_error(fmt::format("Error reading from file: {}", strerror(errno)));
                    else
                        m_file_input_buffer_bytes += (int) read_count;
                } while (read_pos + samples_to_read * m_bytes_per_sample >= m_file_input_buffer_bytes);
            }
            read_pos += samples_to_read * m_bytes_per_sample;
        } else {
            if (read_pos + samples_to_read * m_bytes_per_sample >= m_file_input_buffer_bytes) {
                // move remaining input to start of buffer
                int start_copy_pos = read_pos - c_input_buffer_lookback * m_bytes_per_sample;
                int n = m_file_input_buffer_bytes - start_copy_pos;
                memmove(m_file_input_buffer, m_file_input_buffer + start_copy_pos, n);
                memmove(m_dropout_input_buffer, m_dropout_input_buffer + start_copy_pos / m_bytes_per_sample, n / m_bytes_per_sample);
                m_file_input_buffer_bytes -= start_copy_pos;
                read_pos = c_input_buffer_lookback * m_bytes_per_sample;

                auto block = m_demodulator->getNextDemodulatedBlock();
                if (block == nullptr) {
                    m_log.info(eInput, "ResamplingInputReader: no more demodulated blocks");
                    return false;
                }
                memcpy(m_file_input_buffer + m_file_input_buffer_bytes, block->video_data, sizeof(block->video_data));
                memcpy(m_dropout_input_buffer + m_file_input_buffer_bytes / m_bytes_per_sample, block->dropouts, sizeof(block->dropouts));
                m_file_input_buffer_bytes += sizeof(block->video_data);
                memcpy(efm_buffer, block->efm_data, sizeof(block->efm_data));
                have_efm = true;
                m_demodulator->returnBlock(block);
            }
            assert(read_pos + samples_to_read * m_bytes_per_sample < m_file_input_buffer_bytes);
            read_pos += samples_to_read * m_bytes_per_sample;
        }

        switch (m_input_format) {
            case eUnsignedByte:
                b0 = m_file_input_buffer[read_pos - 3]; // max lookback
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
            case eFloat:
                b0 = ((float *)(m_file_input_buffer + read_pos))[-3];
                b1 = ((float *)(m_file_input_buffer + read_pos))[-2];
                b2 = ((float *)(m_file_input_buffer + read_pos))[-1];
                b3 = ((float *)(m_file_input_buffer + read_pos))[0];
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

        buffer[sample_ix] = (float)(y * m_output_multiplier + m_output_add);
        dropout_buffer[sample_ix] = m_dropout_input_buffer[read_pos / 4];
    }
    m_t = t;
    m_file_input_buffer_read_pos = read_pos;
    return true;
}
