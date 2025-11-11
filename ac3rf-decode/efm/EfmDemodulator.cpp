//
// Created by Staffan Ulfberg on 11/4/25.
//

#include <cstring>
#include <gnuradio/gr_complex.h>
#include <gnuradio/fft/fft.h>
#include "EfmDemodulator.h"

#include <gnuradio/fft/window.h>

static int fd;

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
        // m_input_filter = {
        //     -0.015154, -0.013301, -0.008419, -0.000273, 0.010968, 0.024698, 0.039891, 0.055181, 0.069003, 0.079769, 0.086068, 0.086854, 0.081595, 0.070366, 0.053867, 0.033357, 0.010522, -0.012721, -0.034438, -0.052903, -0.066786, -0.075287, -0.078200, -0.075898, -0.069251, -0.059478, -0.047979, -0.036148, -0.025210, -0.016098, -0.009374, -0.005222
        // };

        const int fft_size = 256;
        gr::fft::fft<gr_complex, false> fft(fft_size);

        fft.get_inbuf()[0] = 0;
        if (fft_size % 2 == 0)
            fft.get_inbuf()[fft_size / 2] = 0;

        for (int i = 1; i < (fft_size + 1) / 2; i++) {
            gr_complex h;
            float f = (float)input_sample_frequency * i / fft_size;
            if (f < 1.5e6)
                h = (f + 0.1e6f) / 1.6e6f;
            else if (f < 1.6e6)
                h = 1;
            else if (f < 1.8e6)
                h = (1.8e6f - f) / 0.2e6f;
            else
                h = 0;

            fft.get_inbuf()[i] = h;
            fft.get_inbuf()[fft_size - i] = conj(h);
        }
        fft.execute();

        // ensure iFFT is real
        for (int i = 0; i < fft_size; i++)
            assert(abs(fft.get_outbuf()[i].imag()) < 1e-6);

        auto window = gr::fft::window::hann(fft_size);
        for (int i = 0; i < fft_size; i++) {
            int ix = (i + fft_size / 2) % fft_size; // ifftshift
            float v = fft.get_outbuf()[ix].real() * window[i];
            printf("%f ", v);
            m_input_filter.push_back(v);
        }
        printf("\n");
        // Our FIR filter implementation requires the filters to have their coefficients reversed
        std::reverse(m_input_filter.begin(), m_input_filter.end());
    } else {
        m_input_filter.push_back(1.0); // minimal no-op filter
    }
    // TODO: maybe pad filter for SIMD

    m_input_filter_buffer = new float[m_input_block_size + m_input_filter.size() - 1];
    m_filtered_input = new float[m_input_block_size];
    m_max_reclocked_size = (int)(m_input_block_size / m_input_sample_frequency * 4321800 * 1.1);
    m_reclocked_data = new bool[m_max_reclocked_size];
    m_max_output_samples = (m_max_reclocked_size / 588 + 1) * 6;

    fd = open("debug.bin",
        O_WRONLY | O_TRUNC | O_CREAT,
        S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
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

    write(fd, m_filtered_input, m_input_block_size * sizeof(float));

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
