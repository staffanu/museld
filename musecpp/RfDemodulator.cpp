//
// Created by staffanu on 12/10/23.
//

#include <cmath>
#include <thread>
#include <complex>
#include <vector>
#include <iostream>
#include <array>
#include "RfDemodulator.h"

using namespace std;

// The filters assume an input sampling rate of 62.5 MHz.  In order to allow that to be set in runtime
// we could link against the gnuradio libraries.
constexpr int decimation_rate = 2;
constexpr int sample_block_size = 4096;

// FIR passband filter to filter out noise over 22.5 MHz and also the pilot signal and EFM.
// The pilot signal isn't completely supressed but that doesn't seem to matter.
// Computed using gnuradio:
// firdes.complex_band_pass(1.0, 62.5e6, 2.7e6, 22.3e6, 2e6, window.WIN_RECTANGULAR)
const vector<complex<float>> bandpass_filter_def = {
        complex<float>(0.0065876576118171215, 0.020274756476283073),
        complex<float>(-0.004714661277830601, 0.00342539488337934),
        complex<float>(0.01447536051273346, 0.010516997426748276),
        complex<float>(-0.00879573542624712, 0.02707030437886715),
        complex<float>(-0.013138349168002605, -3.2994755372328655e-08),
        complex<float>(0.005763731896877289, 0.017739124596118927),
        complex<float>(-0.03205157443881035, 0.02328665927052498),
        complex<float>(-0.021089427173137665, -0.015322495251893997),
        complex<float>(-0.005935038439929485, 0.018265916034579277),
        complex<float>(-0.061971265822649, -2.5511641865705315e-07),
        complex<float>(-0.017557775601744652, -0.05403804033994675),
        complex<float>(-0.015810636803507805, 0.01148699875921011),
        complex<float>(-0.11814974993467331, -0.0858415812253952),
        complex<float>(0.08165450394153595, -0.2513030767440796),
        complex<float>(0.3123723268508911, 1.3405565368884709e-06),
        complex<float>(0.08165234327316284, 0.2513037919998169),
        complex<float>(-0.1181504875421524, 0.08584056794643402),
        complex<float>(-0.015810538083314896, -0.011487134732306004),
        complex<float>(-0.01755823940038681, 0.05403788760304451),
        complex<float>(-0.061971265822649, -2.76787233133291e-07),
        complex<float>(-0.005934881512075663, -0.018265968188643456),
        complex<float>(-0.02108955755829811, 0.015322314575314522),
        complex<float>(-0.03205139935016632, -0.023286905139684677),
        complex<float>(0.005763866938650608, -0.017739081755280495),
        complex<float>(-0.013138349168002605, -6.724289391968341e-08),
        complex<float>(-0.008795528672635555, -0.02707037143409252),
        complex<float>(0.014475440606474876, -0.010516887530684471),
        complex<float>(-0.0047146352007985115, -0.0034254309721291065),
        complex<float>(0.006587812211364508, -0.020274706184864044),
        complex<float>(0, 0), // make size multiple of 8
        complex<float>(0, 0), // make size multiple of 8
        complex<float>(0, 0), // make size multiple of 8
};
constexpr int bandpass_filter_size = 32;

// FIR lowpass filter for the demodulated signal
// Computed using gnuradio: firdes.low_pass(1.0, 62.5e6, 9.8e6, 2e6, window.WIN_RECTANGULAR)
vector<float> lowpass_filter = {
        0.021318137645721436,
        0.005827637854963541,
        -0.01789254881441593,
        -0.028463421389460564,
        -0.013138349168002605,
        0.01865200139582157,
        0.03961782529950142,
        0.02606804110109806,
        -0.01920594647526741,
        -0.061971265822649,
        -0.0568188801407814,
        0.019542962312698364,
        0.14604157209396362,
        0.26423606276512146,
        0.3123723268508911,
        0.26423606276512146,
        0.14604157209396362,
        0.019542962312698364,
        -0.0568188801407814,
        -0.061971265822649,
        -0.01920594647526741,
        0.02606804110109806,
        0.03961782529950142,
        0.01865200139582157,
        -0.013138349168002605,
        -0.028463421389460564,
        -0.01789254881441593,
        0.005827637854963541,
        0.021318137645721436,
        0,
        0,
        0,
};
constexpr int lowpass_filter_size = 32;

// Root raised cosine filter for pulse shaping
// Notice that the root raised cosine filter works on half the sample rate, i.e., 31.25 MHz
// Computed in gnuradio using RRC filter taps with Sample rate 31.25 MHz, Symbol rate 16.2 MHz, Excess BW 0.1.
vector<float> rrc_filter = vector<float>{
    0.048089, 0.028415, -0.092251, -0.029997, 0.296034, 0.499418, 0.296034, -0.029997, -0.092251, 0.028415, 0.048089, 0, 0, 0, 0, 0
};
constexpr int rrc_filter_size = 16;

#define AVX_FLOATS 8

#ifdef __AVX__
#include <immintrin.h>
#include <numeric>

void firFilter(
        const float *input,   // input signal of length at least output_size + filter_length - 1
        size_t output_size,   // number of output values to compute
        float *filter,        // reversed filter coefficients
        size_t filter_size,
        float *output) {
    array<float, AVX_FLOATS> tmp_store{};

    for (int i = 0; i < output_size; i++) {
        __m256 out_chunk = _mm256_setzero_ps();
        for (int j = 0; j < filter_size; j += AVX_FLOATS) {
            __m256 input_chunk = _mm256_loadu_ps(input + i + j);
            __m256 filter_chunk = _mm256_loadu_ps(filter + j);
            out_chunk = _mm256_add_ps(out_chunk, _mm256_mul_ps(input_chunk, filter_chunk));
        }
        _mm256_storeu_ps(tmp_store.data(), out_chunk);
        output[i] = std::accumulate(tmp_store.begin(), tmp_store.end(), 0.f);
    }
}

#else
#warning "AVX not detected"
void firFilter(
        const float *input,   // input signal of length output_length + filter_length - 1
        size_t output_length, // usable input (not including the filter_length-1 extra values)
        const float *filter,  // reversed filter coefficients
        size_t filter_length,
        float *output) {
    for (auto i = 0; i < output_length; i++) {
        output[i] = 0;
        for (auto j = 0; j < filter_length; j++) {
            output[i] += input[i + j] * filter[j];
        }
    }
}
#endif

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

    assert(sample_block_size % decimation_rate == 0);

    assert(bandpass_filter_size % AVX_FLOATS == 0);
    assert(bandpass_filter_size == bandpass_filter_def.size()); // TODO: figure out a way to calculate the size in compile time
    vector<float> bandpass_filter_re;
    vector<float> bandpass_filter_im;
    transform(bandpass_filter_def.rbegin(), bandpass_filter_def.rend(), back_inserter(bandpass_filter_re), [](complex<float> c) -> float { return c.real(); });
    transform(bandpass_filter_def.rbegin(), bandpass_filter_def.rend(), back_inserter(bandpass_filter_im), [](complex<float> c) -> float { return c.imag(); });

    constexpr int input_buffer_size = sample_block_size + bandpass_filter_size - 1;

    float input_buffer[input_buffer_size];
    m_tmp_input_buffer = (int16_t *)malloc(input_buffer_size * sizeof(int16_t)); // FIXME

    float analytic_buffer_re[sample_block_size];
    float analytic_buffer_im[sample_block_size];

    assert(lowpass_filter_size % AVX_FLOATS == 0);
    assert(lowpass_filter_size == lowpass_filter.size()); // TODO: figure out a way to calculate the size in compile time

    constexpr int lowpass_in_buffer_size = sample_block_size + lowpass_filter_size - 1;
    float lowpass_in_buffer[lowpass_in_buffer_size];
    float lowpass_out_buffer[sample_block_size];

    assert(rrc_filter_size % AVX_FLOATS == 0);
    assert(rrc_filter_size == rrc_filter_size.size()); // TODO: figure out a way to calculate the size in compile time

    constexpr int rrc_in_buffer_size = sample_block_size / decimation_rate + rrc_filter_size - 1;
    float rrc_in_buffer[rrc_in_buffer_size];
    float rrc_out_buffer[sample_block_size / decimation_rate];

    constexpr int output_buffer_size = sample_block_size / decimation_rate;
    int16_t output_buffer[output_buffer_size];

    float prev_angle;
    auto t0 = chrono::high_resolution_clock::now();

    if (!readFloats(input_buffer, bandpass_filter_size - 1))
        throw runtime_error("input file too short");
    long sample_counter = bandpass_filter_size - 1;

    while (readFloats(input_buffer + bandpass_filter_size - 1, sample_block_size) && sample_counter < 62500000 * 10) {
        sample_counter += sample_block_size;

        firFilter(input_buffer, sample_block_size, bandpass_filter_re.data(), bandpass_filter_size, analytic_buffer_re);
        firFilter(input_buffer, sample_block_size, bandpass_filter_im.data(), bandpass_filter_size, analytic_buffer_im);

        for (int index = 0; index < sample_block_size; index++) {
            float angle = atan2(analytic_buffer_im[index], analytic_buffer_re[index]);
            float phase_diff = angle > prev_angle ? angle - prev_angle : 2 * (float)M_PI + angle - prev_angle;
            prev_angle = angle;
            float fm_out = (phase_diff * (62.5e6f / (2.f * (float)M_PI * 12.5e6f)) - 1.f) * (12.5e6f / 1.9e6f);
            lowpass_in_buffer[index + lowpass_filter_size - 1] = fm_out;
        }

        firFilter(lowpass_in_buffer, sample_block_size, lowpass_filter.data(), lowpass_filter_size, lowpass_out_buffer);

        for (int index = 0, rcc_index = rrc_filter_size - 1; index < sample_block_size; index += decimation_rate, rcc_index++) {
            rrc_in_buffer[rcc_index] = lowpass_out_buffer[index];
        }

        firFilter(rrc_in_buffer, sample_block_size / 2, rrc_filter.data(), rrc_filter_size, rrc_out_buffer);

        for (int index = 0; index < output_buffer_size; index++) {
            auto value = (int16_t)clamp(rrc_out_buffer[index] * 15000.0, -32768.0, 32767.0);
            output_buffer[index] = value;
        }

        if (write(m_out_file_fd, output_buffer, output_buffer_size * sizeof(int16_t)) != output_buffer_size * sizeof(int16_t))
            throw runtime_error("write failed");

        memcpy(input_buffer, input_buffer + input_buffer_size - bandpass_filter_size + 1, (bandpass_filter_size - 1) * sizeof(float));
        memcpy(lowpass_in_buffer, lowpass_in_buffer + lowpass_in_buffer_size - lowpass_filter_size + 1, (lowpass_filter_size - 1) * sizeof(float));
        memcpy(rrc_in_buffer, rrc_in_buffer + rrc_in_buffer_size - rrc_filter_size + 1, (rrc_filter_size - 1) * sizeof(float));
    }

    auto t1 = chrono::high_resolution_clock::now();
    auto time_us = (double) chrono::duration_cast<chrono::microseconds>(t1 - t0).count();
    m_log.info(ePerformance, fmt::format("Total time {:.3f} s", time_us / 1e6));
}

bool RfDemodulator::readFloats(float *out, size_t n) {
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
