//
// Created by staffanu on 11/11/25.
//

#ifndef AC3RF_DECODE_GARDNERTIMINGRECOVERY_H
#define AC3RF_DECODE_GARDNERTIMINGRECOVERY_H

#include <cassert>
#include <cmath>

#include "../Logger.h"
#include "FractionalResampler.h"
#include "AdaptiveFilter.h"

class TimingRecovery {
public:
    TimingRecovery(Logger &log, double input_sample_frequency, int input_block_size);

    TimingRecovery(const TimingRecovery &) = delete;
    TimingRecovery &operator=(const TimingRecovery &) = delete;
    TimingRecovery(TimingRecovery &&) = delete;
    TimingRecovery &operator=(TimingRecovery &&) = delete;

    int reclock(const float *input, bool *output, int max_output_size);

private:
    Logger &m_log;
    const double m_input_sample_frequency;
    const int m_input_block_size;

    FractionalResampler m_resampler;
    AdaptiveFilter m_filter;

    static constexpr double c_omega = 2 * M_PI * 1200; // un-dampened frequency
    static constexpr double c_zeta = 0.6; // damping factor
    static constexpr double c_Ts = 1 / 4.3218e6;
    const double c_G1 = 1 - exp(-2 * c_zeta * c_omega * c_Ts);
    const double c_G2 = 1 + exp(-2 * c_omega * c_zeta * c_Ts) - 2 * exp(-c_omega * c_zeta * c_Ts) * cos(c_omega * c_Ts * sqrt(1 - c_zeta * c_zeta));

    const double m_nominal_step_size;
    const double m_GpdGvco;
    const double m_g1; // P
    const double m_g2; // I

    double m_step_size_adjustment;
    double m_error_sum;
};

#endif //AC3RF_DECODE_GARDNERTIMINGRECOVERY_H