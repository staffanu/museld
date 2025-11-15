//
// Created by Staffan Ulfberg on 11/4/25.
//

#include <iostream>
#include <gnuradio/gr_complex.h>
#include <gnuradio/fft/fft.h>
#include <gnuradio/fft/window.h>
#include <gnuradio/filter/firdes.h>
#include "EfmDemodulator.h"

static int fd;


static double evalLagrange(const double x[4], int j, double p) {
    double prod = 1;
    for (int i = 0; i < 4; i++) {
        if (i != j) prod *= (p - x[i]) / (x[j] - x[i]);
    }
    return prod;
}

static double interpolate(const double x[4], const double y[4], double p) {
    double s = 0;
    for (int i = 0; i < 4; i++) {
        s += evalLagrange(x, i, p) * y[i];
    }
    return s;
}

static double interpolate(int n, const double x[], const double y[], double p) {
    int first_larger_than_p_ix = -1;
    for (int i = 0; i < n; i++)
        if (x[i] > p) {
            first_larger_than_p_ix = i;
            break;
        }
    assert(first_larger_than_p_ix != -1);
    int start = std::min(n - 4, std::max(0, first_larger_than_p_ix - 2));
    return interpolate(x + start, y + start, p);
}

EfmDemodulator::EfmDemodulator(Logger &log, double input_sample_frequency, int input_block_size, int output_fd,
                               bool rf_input, bool use_simd)
    : m_log(log),
      m_input_sample_frequency(input_sample_frequency),
      m_input_block_size(input_block_size),
      m_output_fd(output_fd),
      m_rf_input(rf_input),
      m_use_simd(use_simd),
      m_efm_pll(log, input_sample_frequency),
      m_gardner_timing_recovery(log, input_sample_frequency, input_block_size),
      m_efm_decoder(log) {

    if (m_rf_input) {
        const int response_table_size = 9;
        const double frequencies[response_table_size] = {0.0e6, 0.25e6, 0.5e6, 0.75e6, 1.0e6, 1.25e6, 1.5e6, 1.75e6, 2.0e6};
        const double amplitude_response[response_table_size] = {0.0, 0.9705, 0.6962, 0.9912, 0.4231, 0.9197, 0.8901, 0.2975, 0.0};
        const double phase_response[response_table_size] = {0.0, -1.9174, -1.1931, -1.4658, -1.6696, -0.7053, -0.9591, -1.2714, -1.0 };
        assert(frequencies[response_table_size - 1] != 0);

        const int fft_size = 256;
        gr::fft::fft<gr_complex, false> fft(fft_size);

        fft.get_inbuf()[0] = 0.f;
        if (fft_size % 2 == 0)
            fft.get_inbuf()[fft_size / 2] = 0;

        log.debug(eAudio, "Desired input filter frequency response:");
        for (int i = 1; i < (fft_size + 1) / 2; i++) {
            float f = (float)input_sample_frequency * i / fft_size;
            gr_complex h = f < frequencies[response_table_size - 1] ? std::polar<float>(
                interpolate(response_table_size, frequencies, amplitude_response, f),
                interpolate(response_table_size, frequencies, phase_response, f)) : std::complex<float>(0.f);

            if (f < 2.2e6) log.debug(eAudio, std::format("{:.0}: {} {}", f, abs(h), arg(h)));

            fft.get_inbuf()[i] = h;
            fft.get_inbuf()[fft_size - i] = conj(h);
        }
        fft.execute();
        for (int i = 0; i < fft_size; i++)
            assert(abs(fft.get_outbuf()[i].imag()) < 1e-6);

        int filter_size = 64;
        auto window = gr::fft::window::hamming(filter_size);
        for (int i = 0; i < filter_size; i++) {
            int ix = (i + fft_size - filter_size / 2) % fft_size; // ifftshift
            float v = fft.get_outbuf()[ix].real() * window[i] / fft_size;
            m_input_filter.push_back(v);
        }
        // Our FIR filter implementation requires the filters to have their coefficients reversed
        std::reverse(m_input_filter.begin(), m_input_filter.end());
    } else {
        m_input_filter = gr::filter::firdes::low_pass_2(1.0, m_input_sample_frequency, 1.7e6, 0.7e6, 40, gr::fft::window::WIN_HAMMING);
    }

    std::ostringstream ss;
    ss << std::format("Input filter size {}: ", m_input_filter.size());
    for (int i = 0; i < m_input_filter.size(); i++)
        ss << " " << m_input_filter[i];;
    m_log.debug(eAudio, ss.str());

    // TODO: maybe pad filter for SIMD

    m_input_filter_buffer = new float[m_input_block_size + m_input_filter.size() - 1];
    m_filtered_input = new float[m_input_block_size];
    m_max_reclocked_size = (int)(m_input_block_size / m_input_sample_frequency * 4321800 * 1.1);
    m_reclocked_data = new bool[m_max_reclocked_size];
    m_max_output_samples = (m_max_reclocked_size / 588 + 1) * 6;

    fd = open("debug.bin", O_WRONLY | O_TRUNC | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
}

EfmDemodulator::~EfmDemodulator() {
    close(fd);
    delete[] m_input_filter_buffer;
    delete[] m_filtered_input;
    delete[] m_reclocked_data;
}

static float prev_x = 0;
static float prev_y = 0;

std::vector<TwoChannelSample> EfmDemodulator::demodulate(float *input_buffer) {
//    memcpy(m_input_filter_buffer + m_input_filter.size() - 1, input_buffer, sizeof(float) * m_input_block_size);

    for (int i = 0; i < m_input_block_size; i++) {
        float y = prev_y * 0.999f + input_buffer[i] - prev_x;
        m_input_filter_buffer[i + m_input_filter.size() - 1] = y;
        prev_x = input_buffer[i];
        prev_y = y;
    }

    FirFilterStage::firFilter(m_input_filter_buffer, m_input_block_size, m_input_filter.data(),
                              m_input_filter.size(), m_filtered_input, 1, false);

    memmove(m_input_filter_buffer, m_input_filter_buffer + m_input_block_size,
            (m_input_filter.size() - 1) * sizeof(float));

    int r = write(fd, m_filtered_input, m_input_block_size * sizeof(float));

//    const int reclocked_bytes = m_efm_pll.reclock(m_filtered_input, m_input_block_size, m_reclocked_data, m_max_reclocked_size);
    const int reclocked_bytes = m_gardner_timing_recovery.reclock(m_filtered_input, m_reclocked_data, m_max_reclocked_size);

    int actual_output_sample_count;
    m_output_samples.resize(m_max_output_samples);
    m_efm_decoder.decode(m_reclocked_data, reclocked_bytes,
        m_max_output_samples, &actual_output_sample_count, m_output_samples.data(), false);
    m_output_samples.resize(actual_output_sample_count);

    return m_output_samples;
}

std::string EfmDemodulator::reedSolomonStatistics() const {
    return m_efm_decoder.reedSolomonStatistics();
}
