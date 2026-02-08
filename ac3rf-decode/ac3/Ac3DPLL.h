//
// Created by Staffan Ulfberg on 10/9/25.
//

#ifndef AC3RF_DECODE_AC3DPLL_H
#define AC3RF_DECODE_AC3DPLL_H

#include "../Logger.h"

class Ac3DPLL {
public:
    Ac3DPLL(Logger &log, double input_sample_frequency, int input_block_size);

    std::vector<uint8_t> reclockSymbols(const std::vector<float> &input_i, const std::vector<float> &input_q);

private:
    static uint8_t decode(std::complex<float> sample);

    Logger &m_log;
    double m_input_sample_frequency;
    int m_input_block_size;
    int m_symbol_distance;

    // We have m_symbol_distance of the input data from the last call so we can
    // compute the differential symbol for the first input data
    std::vector<std::complex<float>> m_prev_input;

    // state for symbol reclocking
    //    val omega = 2 * math.Pi * 1800 // undamped frequency
    //    val zeta = 1.0 // damping factor
    //    val g1 = 1 - math.exp(-2 * zeta * omega / 288000)
    //    val g2 = 1 + math.exp(-2 * omega * zeta / 288000) - 2 * math.exp(-omega * zeta / 288000) * math.cos(omega / 288000 * math.sqrt(1 - zeta * zeta));
    double g1 = 1 / 16.0;
    double g2 = 1 / 512.0; // these constants yield approximately a natural frequency of 2060 Hz and damping factor 0.72

    // we get a new sample with frequency 16 * 2.88 MHz = 46.08 MHz
    constexpr static int c_nominal_symbol_frequency = 576000 / 2;

    uint8_t m_prev_symbol = 0;
    int errorSum = 0;
    int errorSumBits = 15;
    int filterOut = 0; // need to keep this under nominalAdd to ensure counter is strictly increasing
    int counterBits = 10;
    int clkCounter = 0;
    int togglePosition = 0;
    int number_of_toggles = 0;
};


#endif //AC3RF_DECODE_AC3DPLL_H
