// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#include <unistd.h>
#include <cstring>
#include <vector>
#include <cmath>
#include <complex>
#include "Ac3DPLL.h"

Ac3DPLL::Ac3DPLL(Logger &log, double input_sample_frequency) :
    m_log(log),
    m_input_sample_frequency(input_sample_frequency),
    m_symbol_distance(std::round(m_input_sample_frequency / 288e3))
{
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
    int nominalAdd = (int)((1 << counterBits) * (double)c_nominal_symbol_frequency / m_input_sample_frequency);

    for (int i = 0; i < (int)input_i.size(); i++) {
        // The current symbol is computed from the phase difference between the current IQ values, and that
        // symbol_distance samples back.
        uint8_t current_symbol = decode(std::complex<float>(input_i[i], input_q[i])
            * conj(i >= m_symbol_distance ? std::complex<float>(input_i[i - m_symbol_distance], input_q[i - m_symbol_distance]) : m_prev_input[i]));

        if (current_symbol != m_prev_symbol) {
            togglePosition = clkCounter;
            number_of_toggles++;
        }
        m_prev_symbol = current_symbol;

        int newCounter = (clkCounter + nominalAdd + filterOut) & ((1 << counterBits) - 1);
        filterOut = 0;
        if (newCounter < clkCounter) {
            output.push_back(m_prev_symbol);

            int error = number_of_toggles == 1 ? -(togglePosition - (1 << (counterBits - 1))) : 0;

            errorSum = (errorSum + error) << (32 - errorSumBits) >> (32 - errorSumBits); // truncate and sign extend
            filterOut = error * g1 + errorSum * g2;
            number_of_toggles = 0;
        }

        clkCounter = newCounter;
    }
    m_prev_input.clear();
    const int n = (int)input_i.size();
    for (int i = 0; i < m_symbol_distance; i++)
        m_prev_input.push_back(std::complex<float>(input_i[n - m_symbol_distance + i], input_q[n - m_symbol_distance + i]));

    return output;
}
