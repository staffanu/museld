//
// Created by staffanu on 11/11/25.
//

#include <sys/fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <format>
#include <stdexcept>

#ifndef O_BINARY
#  define O_BINARY 0
#endif

#include "TimingRecovery.h"
#include "AdaptiveFilterImpl.h"


TimingRecovery::TimingRecovery(Logger &log, double input_sample_frequency, int input_block_size,
                               int adaptive_filter_size, std::optional<std::string> retiming_debug_filename)
  : m_log(log),
    m_input_sample_frequency(input_sample_frequency),
    m_input_block_size(input_block_size),
    m_resampler(input_block_size),
    m_filter(adaptive_filter_size != 0 ? (AdaptiveFilter *)new AdaptiveFilterImpl(adaptive_filter_size, 1e-3f) : (AdaptiveFilter *)new NonAdaptiveFilter()),
    m_power_estimate(1),
    m_total_symbols(0),
    m_last_adaptive_filter_log(0),
    m_nominal_step_size(input_sample_frequency / 4.3218e6),
    m_GpdGvco(0.5 / m_nominal_step_size),
    m_g1(c_G1 / m_GpdGvco),
    m_g2(c_G2 / m_GpdGvco),
    m_step_size_adjustment(0),
    m_error_sum(0),
    m_prev_sample(0) {

    if (retiming_debug_filename.has_value()) {
        m_debug_fd = open(retiming_debug_filename->c_str(), O_WRONLY | O_TRUNC | O_CREAT | O_BINARY, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (m_debug_fd == -1)
            throw std::runtime_error(std::format("Error writing to output: {}", strerror(errno)));
    }
}

TimingRecovery::~TimingRecovery() {
    delete m_filter;
}

void TimingRecovery::reclock(const float *input, std::vector<float> &output) {

    m_resampler.updateInput(input);

    output.clear();
    float s;
    while (m_resampler.advanceTimeAndResample(m_nominal_step_size + m_step_size_adjustment, s)) {
        m_total_symbols++;
        float power_alpha = m_total_symbols < 5000 ? 0.1f : 0.0001f;

        m_power_estimate = (1 - power_alpha) * m_power_estimate + power_alpha * s * s;
        float gain = sqrt(1 / (m_power_estimate + 1.0));
        m_filter->addSample(s * gain);

        // Initially gain is uncertain and also the signal is not DC
        if (m_total_symbols < 5000)
            continue;

        float sample = m_filter->getOutput();

        // error positive: we're sampling too early, so increase sample spacing
        // Mueller and Muller timing detector
        double error = (m_prev_sample > 0 ? 1. : -1.) * sample - (sample > 0 ? 1. : -1.) * m_prev_sample;
        //printf("%f %f %f  error=%f sum=%f\n", sample_before, sample, sample_after, error, m_error_sum);
        error = std::max(std::min(error, m_nominal_step_size / m_g1 * 0.02), -m_nominal_step_size / m_g1 * 0.02);
        m_error_sum += error;
        m_error_sum = std::max(std::min(m_error_sum, m_nominal_step_size / m_g2 * 0.02), -m_nominal_step_size / m_g2 * 0.02);
        m_step_size_adjustment = error * m_g1 + m_error_sum * m_g2;
        assert(!std::isnan(m_step_size_adjustment) && abs(m_step_size_adjustment) <= m_nominal_step_size * 0.04);

        m_filter->adaptError(sample > 0 ? 1.f : -1.f, sample);

        if (m_debug_fd.has_value()) {
            float debug_floats[2] = { sample, (float)error };
            int r = write(m_debug_fd.value(), debug_floats, 2 * sizeof(float));
            if (r == -1)
                throw std::runtime_error(std::format("Error writing to output: {}", strerror(errno)));
        }

        output.emplace_back(sample);
        m_prev_sample = sample;
    }

    if (m_filter->size() != 0 && m_total_symbols - m_last_adaptive_filter_log > 4321800) {
        m_log.debug(eAudio, "Adaptive filter coefficients: " + m_filter->filterString());
        m_last_adaptive_filter_log = m_total_symbols;
    }
}
