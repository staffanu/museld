// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <unistd.h>
#include <cassert>
#include <vector>
#include <cmath>
#include <complex>
#include "Ac3DPLL.h"

Ac3DPLL::Ac3DPLL(Logger &log, double input_sample_frequency) :
    m_log(log),
    m_input_sample_frequency(input_sample_frequency),
    m_symbol_distance(std::round(m_input_sample_frequency / c_nominal_symbol_frequency)),
    m_G1(c_g1 / m_GpdGvco),
    m_G2(c_g2 / m_GpdGvco),
    m_nominal_add((1 << c_counter_bits) * c_nominal_symbol_frequency / input_sample_frequency)
{
    assert(m_symbol_distance < (int)(input_sample_frequency / c_nominal_symbol_frequency) * 4);
    m_prev_input.resize(m_symbol_distance);
}

uint8_t Ac3DPLL::decode(std::complex<float> sample) {
    const float i = sample.real();
    const float q = sample.imag();

    if (std::fabs(i) >= std::fabs(q))
        return i >= 0.f ? 0 : 3;
    else
        return q >= 0.f ? 1 : 2;
}

std::vector<uint8_t> Ac3DPLL::reclockSymbols(const std::vector<float> &input_i, const std::vector<float> &input_q) {
    std::vector<uint8_t> output;

    for (int i = 0; i < (int)input_i.size(); i++) {
        // The current symbol is computed from the phase difference between the current IQ values, and that
        // symbol_distance samples back.
        uint8_t current_symbol = decode(std::complex<float>(input_i[i], input_q[i])
            * conj(i >= m_symbol_distance ? std::complex<float>(input_i[i - m_symbol_distance], input_q[i - m_symbol_distance]) : m_prev_input[i]));

        if (current_symbol != m_prev_symbol) {
            m_toggle_position = m_clk_counter;
            m_number_of_toggles++;
        }
        m_prev_symbol = current_symbol;

        int newCounter = (m_clk_counter + m_nominal_add + m_filter_out) & ((1 << c_counter_bits) - 1);
        m_filter_out = 0;
        if (newCounter < m_clk_counter) {
            output.push_back(m_prev_symbol);

            int error = m_number_of_toggles == 1 ? -(m_toggle_position - (1 << (c_counter_bits - 1))) : 0;

            m_error_sum = (m_error_sum + error) << (32 - c_error_sum_bits) >> (32 - c_error_sum_bits); // truncate and sign extend
            m_filter_out = error * m_G1 + m_error_sum * m_G2;
            m_number_of_toggles = 0;
        }

        m_clk_counter = newCounter;
    }
    m_prev_input.clear();
    const int n = (int)input_i.size();
    for (int i = 0; i < m_symbol_distance; i++)
        m_prev_input.push_back(std::complex<float>(input_i[n - m_symbol_distance + i], input_q[n - m_symbol_distance + i]));

    return output;
}
