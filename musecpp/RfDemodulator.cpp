//
// Created by staffanu on 12/10/23.
//

#include <cmath>
#include <thread>
#include <vector>
#include <iostream>
#include "RfDemodulator.h"

using namespace std;

bool RfDemodulator::initialize() {
    m_input_fd = open(m_filename.c_str(), O_NONBLOCK);
    if (m_input_fd == -1)
        throw runtime_error("Unable to open file");

#ifdef linux
    if (m_input_is_fifo) {
        m_log.debug(eInput, fmt::format("Pipe size: {}", fcntl(m_input_fd, F_GETPIPE_SZ)));
        fcntl(m_input_fd, F_SETPIPE_SZ, 1024 * 1024);
        m_log.debug(eInput, fmt::format("Pipe size now: {}", fcntl(m_input_fd, F_GETPIPE_SZ)));
    }
#endif

    return true;
}

void RfDemodulator::cleanup() {
    close(m_input_fd);
}

void RfDemodulator::demodulate() {
    auto m_out_file_fd = open("outfile",  O_WRONLY | O_TRUNC | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (m_out_file_fd == -1)
        throw runtime_error("Unable to open output file");

    assert(c_sample_block_size % c_decimation_rate == 0);
    assert(c_bandpass_filter_size % c_AVX_floats_per_chunk == 0);
    assert(c_lowpass_filter_size % c_AVX_floats_per_chunk == 0);
    assert(c_rrc_filter_size % c_AVX_floats_per_chunk == 0);

    float input_buffer[c_input_buffer_size];
    float analytic_buffer_re[c_sample_block_size];
    float analytic_buffer_im[c_sample_block_size];
    float lowpass_in_buffer[c_lowpass_in_buffer_size];
    float lowpass_out_buffer[c_sample_block_size];
    float rrc_in_buffer[c_rrc_in_buffer_size];
    float rrc_out_buffer[c_sample_block_size / c_decimation_rate];
    int16_t output_buffer[c_output_buffer_size];

    float prev_angle;
    auto t0 = chrono::high_resolution_clock::now();

    if (!readFloats(input_buffer, c_bandpass_filter_size - 1))
        throw runtime_error("input file too short");
    long sample_counter = c_bandpass_filter_size - 1;

    while (readFloats(input_buffer + c_bandpass_filter_size - 1, c_sample_block_size) && sample_counter < 62500000 * 10) {
        sample_counter += c_sample_block_size;

        // First run the input signal through the bandpass filter that also converts the signal into an analytic signal
        firFilter(input_buffer, c_sample_block_size, c_bandpass_filter.first, 1, analytic_buffer_re);
        firFilter(input_buffer, c_sample_block_size, c_bandpass_filter.second, 1, analytic_buffer_im);

        // Demodulate the analytic signal
        for (int index = 0; index < c_sample_block_size; index++) {
            float angle = atan2(analytic_buffer_im[index], analytic_buffer_re[index]);
            float phase_diff = angle - prev_angle + (float)(angle < prev_angle) * c_2pi;
            prev_angle = angle;
            // fm_out will be in the range [-1, 1] when the frequency varies between c_center_frequency - c_frequency_deviation and c_center_frequency + c_frequency_deviation
            // This expression is probably easier to understand but has one more multiplication:
            // (phase_diff * (c_sample_frequency / (c_2pi * c_center_frequency)) - 1.f) * (c_center_frequency / c_frequency_deviation);
            float fm_out = phase_diff * (c_sample_frequency / (c_2pi * c_frequency_deviation)) - c_center_frequency / c_frequency_deviation;
            lowpass_in_buffer[index + c_lowpass_filter_size - 1] = fm_out;
        }

        // Lowpass filter the demodulated signal, and down-sample before rrc filtering
        firFilter(lowpass_in_buffer, c_sample_block_size / 2, c_lowpass_filter, 2, rrc_in_buffer + c_rrc_filter_size - 1);

        // Run the down-sampled signal through the root raised cosine pulse-shaping filter
        firFilter(rrc_in_buffer, c_sample_block_size / 2, c_rrc_filter, 1, rrc_out_buffer);

        // Convert the output to ints and scale -- this step could be avoided since the decoder starts with converting
        // back to floats
        for (int index = 0; index < c_output_buffer_size; index++) {
            auto value = (int16_t)clamp(rrc_out_buffer[index] * 15000.0, -32768.0, 32767.0);
            output_buffer[index] = value;
        }

        if (write(m_out_file_fd, output_buffer, c_output_buffer_size * sizeof(int16_t)) != c_output_buffer_size * sizeof(int16_t))
            throw runtime_error("write failed");

        // Copy the end of the filter inputs to the start of the corresponding buffers
        memcpy(input_buffer, input_buffer + c_input_buffer_size - c_bandpass_filter_size + 1, (c_bandpass_filter_size - 1) * sizeof(float));
        memcpy(lowpass_in_buffer, lowpass_in_buffer + c_lowpass_in_buffer_size - c_lowpass_filter_size + 1, (c_lowpass_filter_size - 1) * sizeof(float));
        memcpy(rrc_in_buffer, rrc_in_buffer + c_rrc_in_buffer_size - c_rrc_filter_size + 1, (c_rrc_filter_size - 1) * sizeof(float));
    }

    auto t1 = chrono::high_resolution_clock::now();
    auto time_us = (double) chrono::duration_cast<chrono::microseconds>(t1 - t0).count();
    m_log.info(ePerformance, fmt::format("Total time {:.3f} s", time_us / 1e6));
}

bool RfDemodulator::readFloats(float *out, size_t n) {
    int16_t m_tmp_input_buffer[c_input_buffer_size];
    int filled_bytes = 0;
    do {
        ssize_t read_count = read(m_input_fd,
                                  (void *)((char *)m_tmp_input_buffer + filled_bytes),
                                  n * sizeof(int16_t) - filled_bytes);
        if (read_count == -1 && errno == EAGAIN)
            this_thread::sleep_for(chrono::milliseconds(1));
        else if (read_count == 0) {
            if (!m_input_is_fifo)
                return false;
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

#else
#warning "AVX not detected"
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
