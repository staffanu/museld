// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_AC3DPLL_H
#define AC3RF_DECODE_AC3DPLL_H

#include <cmath>
#include <cstdint>
#include <numbers>

#include "logging/Logger.h"

class Ac3DPLL {
public:
    Ac3DPLL(Logger &log, double input_sample_frequency);

    std::vector<uint8_t> reclockSymbols(const std::vector<float> &input_i, const std::vector<float> &input_q);

private:
    static uint8_t decode(std::complex<float> sample);

    Logger &m_log;
    double m_input_sample_frequency;
    int m_symbol_distance;

    // We have m_symbol_distance of the input data from the last call so we can
    // compute the differential symbol for the first input data
    std::vector<std::complex<float>> m_prev_input;

    static constexpr int c_counter_bits = 10;
    constexpr static int c_nominal_symbol_frequency = 576000 / 2;

    // The symbol reclocking loop filter is designed as a second-order loop with natural
    // frequency c_omega and damping factor c_zeta, updated at the symbol rate.
    static constexpr double c_omega = 2 * std::numbers::pi * 1800; // undamped natural frequency [rad/s]
    static constexpr double c_zeta = 0.6; // damping factor
    static constexpr double c_Ts = 1.0 / c_nominal_symbol_frequency;
    const double c_g1 = 1 - std::exp(-2 * c_zeta * c_omega * c_Ts);
    const double c_g2 = 1 + std::exp(-2 * c_omega * c_zeta * c_Ts)
        - 2 * std::exp(-c_omega * c_zeta * c_Ts) * std::cos(c_omega * c_Ts * std::sqrt(1 - c_zeta * c_zeta));

    // Combined phase detector and VCO gain; the detector's average gain is well below unity
    // since only cycles with exactly one symbol transition update the loop.
    const double m_GpdGvco = 0.3;
    const double m_G1; // proportional gain
    const double m_G2; // integral gain

    // nominal counter increment per input sample: 2^c_counter_bits * symbol frequency / sample frequency
    const int m_nominal_add;

    uint8_t m_prev_symbol = 0;
    static constexpr int c_error_sum_bits = 15;
    int m_error_sum = 0;
    int m_filter_out = 0; // need to keep this under nominalAdd to ensure counter is strictly increasing
    int m_clk_counter = 0;
    int m_toggle_position = 0;
    int m_number_of_toggles = 0;
};


#endif //AC3RF_DECODE_AC3DPLL_H
