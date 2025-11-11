//
// Created by staffanu on 11/11/25.
//

#include "GardnerTimingRecovery.h"

GardnerTimingRecovery::GardnerTimingRecovery(Logger &log, double input_sample_frequency, int input_block_size)
  : m_log(log),
    m_input_sample_frequency(input_sample_frequency),
    m_input_block_size(input_block_size),
    m_resampler(input_block_size),
    m_nominal_step_size(input_sample_frequency / 4.3218e6) {


}

int GardnerTimingRecovery::reclock(const float *input, bool *output, int max_output_size) {

    m_resampler.updateInput(input);

    int output_size = 0;
    while (m_resampler.advanceTime(m_nominal_step_size + m_step_size_adjustment, m_nominal_step_size / 2)) {
        float sample = m_resampler.resample(0.0);
        float sample_before = m_resampler.resample(-m_nominal_step_size / 2);
        float sample_after = m_resampler.resample(m_nominal_step_size / 2);

        bool symbol;
        if (sample_before * sample_after < 0) {
            symbol = true;
            float error = sample * (sample_after - sample_before);
            m_error_sum += error;
            m_step_size_adjustment += error / 100 + m_error_sum / 10000;
        } else {
            symbol = false;
        }

        assert(output_size < max_output_size);
        output[output_size++] = symbol;
    }
    
    return output_size;
}
