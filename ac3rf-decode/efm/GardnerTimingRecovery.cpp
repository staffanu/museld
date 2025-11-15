//
// Created by staffanu on 11/11/25.
//

#include "GardnerTimingRecovery.h"

#include <fcntl.h>
#include <fmt/format.h>

static int fd;

static float power_estimate = 1;
static long total_symbols = 0;

GardnerTimingRecovery::GardnerTimingRecovery(Logger &log, double input_sample_frequency, int input_block_size)
  : m_log(log),
    m_input_sample_frequency(input_sample_frequency),
    m_input_block_size(input_block_size),
    m_resampler(input_block_size),
    m_filter(7, 0.000005f),
    m_nominal_step_size(input_sample_frequency / 4.3218e6),
    m_GpdGvco(0.5 / m_nominal_step_size),
    m_g1(c_G1 / m_GpdGvco),
    m_g2(c_G2 / m_GpdGvco),
    m_step_size_adjustment(0),
    m_error_sum(0) {

    fd = open("gardner.bin", O_WRONLY | O_TRUNC | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
}

int GardnerTimingRecovery::reclock(const float *input, bool *output, int max_output_size) {

    m_resampler.updateInput(input);

    int output_size = 0;
    while (m_resampler.advanceTime(m_nominal_step_size + m_step_size_adjustment, (m_nominal_step_size + m_step_size_adjustment) / 2)) {
        total_symbols++;
        float power_alpha = total_symbols < 5000 ? 0.1f : 0.0001f;

        for (int i = 0; i < 2; i++) {
            float s = m_resampler.resample(i * (m_nominal_step_size + m_step_size_adjustment) / 2);
            power_estimate = (1 - power_alpha) * power_estimate + power_alpha * s * s;
            float gain = sqrt(1 / (power_estimate + 1.0));
            m_filter.add_sample(s * gain);
        }

        // Initially gain is uncertain and also the signal is not DC
        if (total_symbols < 5000)
            continue;

        float sample_before, sample, sample_after;
        m_filter.getLast3(sample_before, sample, sample_after);

        //printf("3 samples: %f %f %f\n", sample_before, sample, sample_after);

        float debug_floats[4] = { sample, 1.f, sample_after, 0.f};
        int r = write(fd, debug_floats, 4 * sizeof(float));

        bool symbol;
        if (sample_before * sample_after < 0) {
            symbol = true;

            // error positive: we're sampling too early, so increase sample spacing
            // notice error is normalized dividing by (before - after)^2.
            double error = sample / (sample_before - sample_after);
            //printf("%f %f %f  error=%f\n", sample_before, sample, sample_after, error);
            error = std::max(std::min(error, m_nominal_step_size / m_g1 * 0.02), -m_nominal_step_size / m_g1 * 0.02);
            m_error_sum += error;
            m_error_sum = std::max(std::min(m_error_sum, m_nominal_step_size / m_g2 * 0.02), -m_nominal_step_size / m_g2 * 0.02);
            m_step_size_adjustment = error * m_g1 + m_error_sum * m_g2;
            assert(abs(m_step_size_adjustment) <= m_nominal_step_size * 0.04);
        } else {
            symbol = false;
        }

        float d = symbol ? 0 : sample > 0 ? 1.f : -1.f;
        float e = std::max(std::min(d - sample, 1.f), -1.f);
        m_filter.adaptError(e);

        assert(output_size < max_output_size);
        output[output_size++] = symbol;
    }
    m_filter.printFilter();

    return output_size;
}
