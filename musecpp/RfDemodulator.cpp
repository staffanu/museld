//
// Created by staffanu on 12/10/23.
//

#include <cmath>
#include <thread>
#include <vector>
#include <iostream>
#include <filesystem>
#include "RfDemodulator.h"

using namespace std;

RfDemodulator::RfDemodulator(Logger &log, std::string filename)
: m_log(log),
  m_filename(std::move(filename)),
  m_input_fd(-1),
  m_input_is_fifo(filesystem::is_fifo(filename)),
  m_demodulator_thread(nullptr),
  m_stop_request(false),
  m_reader_thread_finished(false) {
}

bool RfDemodulator::initialize() {
    m_input_fd = open(m_filename.c_str(), O_NONBLOCK);
    if (m_input_fd == -1)
        throw runtime_error(fmt::format("RfDemodulator: Unable to open input file {}", m_filename));

#ifdef linux
    if (m_input_is_fifo) {
        m_log.debug(eInput, fmt::format("Pipe size: {}", fcntl(m_input_fd, F_GETPIPE_SZ)));
        fcntl(m_input_fd, F_SETPIPE_SZ, 1024 * 1024);
        m_log.debug(eInput, fmt::format("Pipe size now: {}", fcntl(m_input_fd, F_GETPIPE_SZ)));
    }
#endif

    for (int i = 0; i < c_number_of_block_buffers; i++)
        m_vacant_blocks.push_back(make_shared<DemodulatedBlock>());

    m_demodulator_thread = new thread(&RfDemodulator::demodulate, this);
#ifdef linux
    pthread_setname_np(m_demodulator_thread->native_handle(), "musecpp-demod");
#endif

    return true;
}

std::shared_ptr<DemodulatedBlock> RfDemodulator::getNextDemodulatedBlock() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv_filled.wait(
            lock,
            [this] { return m_reader_thread_finished || !m_filled_blocks.empty(); });
    if (m_filled_blocks.empty())
        return nullptr;

    auto block = m_filled_blocks.front();
    m_filled_blocks.pop_front();
    return block;
}

void RfDemodulator::returnBlock(std::shared_ptr<DemodulatedBlock> &buffer) {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv_vacant.notify_one();
    m_vacant_blocks.push_back(buffer);
}

void RfDemodulator::seek(double seconds) {
    std::unique_lock<std::mutex> lock(m_mutex);

    off_t samples_to_seek = (off_t) (seconds * c_sample_frequency);
    off_t bytes_to_seek = 2 * samples_to_seek;
    m_log.info(eInput, fmt::format("Seeking relative time {} s, {} samples, {} bytes.",
                                   seconds, samples_to_seek, bytes_to_seek));
    lseek(m_input_fd, bytes_to_seek, SEEK_CUR);
}

void RfDemodulator::cleanup() {
    {
        std::unique_lock<std::mutex> lock(m_mutex);
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

void RfDemodulator::demodulate() {
    assert(c_sample_block_size % c_decimation_rate == 0);
    assert(c_bandpass_filter_size % c_AVX_floats_per_chunk == 0);
    assert(c_lowpass_filter_size % c_AVX_floats_per_chunk == 0);
    assert(c_rrc_filter_size % c_AVX_floats_per_chunk == 0);
    assert(c_output_buffer_size == DemodulatedBlock::c_block_size);
    assert(c_efm_out_buffer_size == DemodulatedBlock::c_efm_block_size);

    float input_buffer[c_input_buffer_size] = {};
    float analytic_buffer_re[c_sample_block_size] = {};
    float analytic_buffer_im[c_sample_block_size] = {};
    float lowpass_in_buffer[c_lowpass_in_buffer_size] = {};
    float rrc_in_buffer[c_rrc_in_buffer_size] = {};
    float efm_equalization_in_buffer[c_efm_equalization_in_buffer_size] = {};
    float dropout_abs_buffer[c_dropout_abs_buffer_size] = {};
    float dropout_out_buffer[c_dropout_out_buffer_size] = {};

    float prev_angle;
    long total_samples_read = 0;
    long total_samples_read_last_log = 0;
    long total_time_since_last_log_us = 0;

    while (!m_stop_request && readFloats(input_buffer + c_bandpass_filter_size - 1, c_sample_block_size)) {
        auto t0 = chrono::high_resolution_clock::now();

        // first get a free output block to write to
        shared_ptr<DemodulatedBlock> block = nullptr;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_input_is_fifo && m_vacant_blocks.empty()) {
                // discard a filled buffer -- this is better than having the writer to the fifo wait
                m_log.warn(eInput, "Discarding demodulated block due to overrun");
                assert(!m_filled_blocks.empty());
                m_vacant_blocks.push_back(m_filled_blocks.back());
                m_filled_blocks.pop_back();
            }
            m_cv_vacant.wait(lock, [this] { return m_stop_request || !m_vacant_blocks.empty(); });
            if (m_stop_request) {
                m_log.info(eInput, "RfDemodulator: stop requested");
                break;
            }
            block = m_vacant_blocks.front();
            m_vacant_blocks.pop_front();
        }

        // First run the input signal through the bandpass filter that also converts the signal into an analytic signal
        firFilter(input_buffer, c_sample_block_size, c_bandpass_filter.first, 1, analytic_buffer_re);
        firFilter(input_buffer, c_sample_block_size, c_bandpass_filter.second, 1, analytic_buffer_im);

        // Demodulate the analytic signal
        for (int index = 0; index < c_sample_block_size; index++) {
            float angle = atan2(analytic_buffer_im[index], analytic_buffer_re[index]);
            float phase_diff = angle - prev_angle + (float) (angle < prev_angle) * c_2pi;
            prev_angle = angle;
            // fm_out will be in the range [-1, 1] when the frequency varies between c_center_frequency - c_frequency_deviation and c_center_frequency + c_frequency_deviation
            // This expression is probably easier to understand but has one more multiplication:
            // (phase_diff * (c_sample_frequency / (c_2pi * c_center_frequency)) - 1.f) * (c_center_frequency / c_frequency_deviation);
            float fm_out = phase_diff * (c_sample_frequency / (c_2pi * c_frequency_deviation)) -
                           c_center_frequency / c_frequency_deviation;
            lowpass_in_buffer[index + c_lowpass_filter_size - 1] = fm_out;
        }

        // Lowpass filter the demodulated signal, and down-sample before rrc filtering
        firFilter(lowpass_in_buffer, c_sample_block_size / 2, c_lowpass_filter, 2,
                  rrc_in_buffer + c_rrc_filter_size - 1);

        // Run the down-sampled signal through the root raised cosine pulse-shaping filter and store in the output block
        firFilter(rrc_in_buffer, c_sample_block_size / 2, c_rrc_filter, 1, block->data);
        for (int i = 0; i < block->c_block_size; i++)
            // +/- 1 corresponds to the white/black level, which is 128+/-112 (16, 240) in MUSE
            block->data[i] = block->data[i] * 112.f + 128.f;

        // EFM
        firFilter(input_buffer, c_sample_block_size / c_efm_decimation_rate, c_efm_lowpass_filter,
                  c_efm_decimation_rate, efm_equalization_in_buffer + c_efm_equalization_filter_size - 1);
        firFilter(efm_equalization_in_buffer, c_efm_out_buffer_size, c_efm_equalization_filter, 1, block->efm_data);


        // Copy the end of the filter inputs to the start of the corresponding buffers
        memcpy(input_buffer, input_buffer + c_input_buffer_size - c_bandpass_filter_size + 1,
               (c_bandpass_filter_size - 1) * sizeof(float));
        memcpy(lowpass_in_buffer, lowpass_in_buffer + c_lowpass_in_buffer_size - c_lowpass_filter_size + 1,
               (c_lowpass_filter_size - 1) * sizeof(float));
        memcpy(rrc_in_buffer, rrc_in_buffer + c_rrc_in_buffer_size - c_rrc_filter_size + 1,
               (c_rrc_filter_size - 1) * sizeof(float));
        memcpy(efm_equalization_in_buffer, efm_equalization_in_buffer + c_efm_equalization_in_buffer_size - c_efm_equalization_filter_size + 1,
               (c_efm_equalization_filter_size - 1) * sizeof(float));

        auto t1 = chrono::high_resolution_clock::now();
        total_time_since_last_log_us += chrono::duration_cast<chrono::microseconds>(t1 - t0).count();

        // Send away result
        block->rf_location = total_samples_read;
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv_filled.notify_one();
        m_filled_blocks.push_back(block);

        total_samples_read += c_sample_block_size;
        if (total_samples_read - total_samples_read_last_log > (long)c_sample_frequency) {
            m_log.info(eInput | ePerformance,
                       fmt::format("Total demodulation time last second: {:.1f} ms", (double)total_time_since_last_log_us / 1000.0));
            total_time_since_last_log_us = 0;
            total_samples_read_last_log = total_samples_read;
        }
    }
    m_reader_thread_finished = true;
}

bool RfDemodulator::readFloats(float *out, size_t n) {
    int16_t m_tmp_input_buffer[n];
    int filled_bytes = 0;
    do {
        if (m_stop_request) {
            m_log.info(eInput, "RfDemodulator: stop requested");
            return false;
        }
        ssize_t read_count = read(m_input_fd,
                                  (void *)((char *)m_tmp_input_buffer + filled_bytes),
                                  n * sizeof(int16_t) - filled_bytes);
        if (read_count == -1 && errno == EAGAIN)
            this_thread::sleep_for(chrono::milliseconds(1));
        else if (read_count == 0) {
            if (!m_input_is_fifo) {
                m_log.info(eInput, "RfDemodulator: end of file");
                return false;
            }
        } else if (read_count == -1)
            throw runtime_error(fmt::format("Error reading from file: {}", strerror(errno)));
        else
            filled_bytes += (int)read_count;
    } while (filled_bytes < n * sizeof(int16_t));

    for (int i = 0; i < n; i++)
        out[i] = (float)m_tmp_input_buffer[i];

    return true;
}

#ifdef __AVX__
#include <immintrin.h>
#include <numeric>

template<size_t filter_size> void RfDemodulator::firFilter(
        const float *input,   // input signal of length at least output_size + filter_length - 1
        size_t output_size,   // number of output values to compute -- must be even!
        const array<float, filter_size> &filter,  // reversed filter coefficients, needs to be aligned at 32 byte multiple (c_AVX_floats_per_chunk * sizeof(float))
        int decimation_rate,
        float *output) {
    assert(output_size % 2 == 0);
    alignas(__m256) array<float, c_AVX_floats_per_chunk> tmp_store0{};
    alignas(__m256) array<float, c_AVX_floats_per_chunk> tmp_store1{};

    for (int oi = 0, ii = 0; oi < output_size; oi += 2, ii += 2 * decimation_rate) {
        __m256 out_chunk0 = _mm256_setzero_ps();
        __m256 out_chunk1 = _mm256_setzero_ps();

        for (int j = 0; j < filter_size; j += c_AVX_floats_per_chunk) {
            __m256 filter_chunk = _mm256_loadu_ps(filter.data() + j);

            __m256 input_chunk0 = _mm256_loadu_ps(input + ii + j);
            out_chunk0 = _mm256_add_ps(out_chunk0, _mm256_mul_ps(input_chunk0, filter_chunk));

            __m256 input_chunk1 = _mm256_loadu_ps(input + ii + decimation_rate + j);
            out_chunk1 = _mm256_add_ps(out_chunk1, _mm256_mul_ps(input_chunk1, filter_chunk));
        }
        _mm256_store_ps(tmp_store0.data(), out_chunk0); // aligned store
        output[oi] = std::accumulate(tmp_store0.begin(), tmp_store0.end(), 0.f);

        _mm256_store_ps(tmp_store1.data(), out_chunk1); // aligned store
        output[oi + 1] = std::accumulate(tmp_store1.begin(), tmp_store1.end(), 0.f);
    }
}

#elif __ARM_NEON == 1
#include <arm_neon.h>

template<size_t filter_size> void RfDemodulator::firFilter(
        const float *input,   // input signal of length at least output_size + filter_length - 1
        size_t output_size,   // number of output values to compute -- must be even!
        const array<float, filter_size> &filter,  // reversed filter coefficients
        int decimation_rate,
        float *output) {
    assert(output_size % 2 == 0);
    alignas(float32x4_t) array<float, c_NEON_floats_per_chunk> tmp_store0{};
    alignas(float32x4_t) array<float, c_NEON_floats_per_chunk> tmp_store1{};

    for (int oi = 0, ii = 0; oi < output_size; oi += 2, ii += 2 * decimation_rate) {
        float32x4_t out_chunk0 = vdupq_n_f32(0);
        float32x4_t out_chunk1 = vdupq_n_f32(0);

        for (int j = 0; j < filter_size; j += c_NEON_floats_per_chunk) {
            float32x4_t filter_chunk = vld1q_f32(filter.data() + j);

            float32x4_t input_chunk0 = vld1q_f32(input + ii + j);
            out_chunk0 = vmlaq_f32(out_chunk0, input_chunk0, filter_chunk);

            float32x4_t input_chunk1 = vld1q_f32(input + ii + decimation_rate + j);
            out_chunk1 = vmlaq_f32(out_chunk1, input_chunk1, filter_chunk);
        }

        output[oi] = vaddvq_f32(out_chunk0);
        output[oi + 1] = vaddvq_f32(out_chunk1);
    }
}

#else
#warning "SIMD architecture not detected"
template<size_t filter_size> void RfDemodulator::firFilter(
        const float *input,   // input signal of length output_length + filter_length - 1
        size_t output_size, // usable input (not including the filter_length-1 extra values)
        const array<float, filter_size> &filter,  // reversed filter coefficients
        int decimation_rate,
        float *output) {
    for (auto i = 0; i < output_size; i++) {
        output[i] = 0;
        for (auto j = 0; j < filter_size; j++) {
            output[i] += input[i * decimation_rate + j] * filter[j];
        }
    }
}
#endif
