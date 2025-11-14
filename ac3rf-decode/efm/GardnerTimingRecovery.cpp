//
// Created by staffanu on 11/11/25.
//

#include "GardnerTimingRecovery.h"

#include <fcntl.h>
#include <fmt/format.h>

static int fd;

GardnerTimingRecovery::GardnerTimingRecovery(Logger &log, double input_sample_frequency, int input_block_size)
  : m_log(log),
    m_input_sample_frequency(input_sample_frequency),
    m_input_block_size(input_block_size),
    m_resampler(input_block_size),
    m_nominal_step_size(input_sample_frequency / 4.3218e6),
    m_GpdGvco(0.5 / m_nominal_step_size),
    m_g1(c_G1 / m_GpdGvco),
    m_g2(c_G2 / m_GpdGvco),
    m_step_size_adjustment(0),
    m_error_sum(0) {

    fd = open("gardner.bin", O_WRONLY | O_TRUNC | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
}

static const int filter_size = 9; // assume odd
static const int window_size = filter_size + filter_size / 2 + 2;
static float window[window_size]; // raw samples -- newest last
static float filter[filter_size] = {0, 0, 0, 0, 1, 0, 0, 0, 0 };
static float filtered[filter_size]; // filtered samples -- newest last

int GardnerTimingRecovery::reclock(const float *input, bool *output, int max_output_size) {

    m_resampler.updateInput(input);

    int output_size = 0;
    while (m_resampler.advanceTime(m_nominal_step_size + m_step_size_adjustment, (m_nominal_step_size + m_step_size_adjustment) / 2)) {
        for (int i = 0; i < window_size - 2; i++)
            window[i] = window[i + 2];
        for (int i = 0; i < 2; i++)
            window[window_size - 2 + i] = m_resampler.resample(i * (m_nominal_step_size + m_step_size_adjustment) / 2);

        for (int i = 0; i < filter_size - 2; i++)
            filtered[i] = filtered[i + 2];
        for (int i = 0; i < 2; i++) {
            float s = 0.f;
            for (int j = 0; j < filter_size; j++)
                s += filter[j] * window[window_size - filter_size - 1 + i + j];
            filtered[filter_size - 2 + i] = s;
        }

        float sample_before = filtered[filter_size / 2 - 1];
        float sample = filtered[filter_size / 2];
        float sample_after = filtered[filter_size / 2 + 1];

        //printf("3 samples: %f %f %f\n", sample_before, sample, sample_after);

        write(fd, &sample, sizeof(float));
        float indicator = 1.f;
        write(fd, &indicator, sizeof(float));
        write(fd, &sample_after, sizeof(float));
        indicator = 0.f;
        write(fd, &indicator, sizeof(float));

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

        float e = symbol ? sample : sample > 0 ? sample - 60.f : sample - -60.f;
        for (int i = 0; i < filter_size; i++) {
            filter[i] -= window[i] * e * 0.0000005f;
        }

        assert(output_size < max_output_size);
        output[output_size++] = symbol;
    }

    for (int i = 0; i < filter_size; i++)
        printf("%f ", filter[i]);
    printf("\n");

    //printf("adj: %f\n", m_step_size_adjustment);
    return output_size;
}
