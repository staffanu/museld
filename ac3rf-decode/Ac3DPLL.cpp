//
// Created by Staffan Ulfberg on 10/9/25.
//

#include <vector>
#include "Ac3DPLL.h"

Ac3DPLL::Ac3DPLL(Logger &log)
: m_log(log)
{
}

int Ac3DPLL::reclockSymbols(double sampling_frequency, const std::vector<uint8_t> &input_symbols, int input_size, std::vector<uint8_t> &output_symbols, int max_output_size) {
    int output_size = 0;
    int nominalAdd = ((1 << counterBits) * (long)c_nominal_symbol_frequency / sampling_frequency);

    for (int i = 0; i < input_size; i++) {
        uint8_t dataIn = input_symbols[i];

        if (dataIn != lastIn) {
            togglePosition = clkCounter;
            number_of_toggles++;
        }
        lastIn = dataIn;

        int newCounter = (clkCounter + nominalAdd + filterOut) & ((1 << counterBits) - 1);
        filterOut = 0;
        if (newCounter < clkCounter) {
            if (output_size == max_output_size) {
                m_log.warn("Symbol output buffer overflow");
                return output_size;
            }
            output_symbols[output_size++] = lastIn;

            int error = number_of_toggles == 1 ? -(togglePosition - (1 << (counterBits - 1))) : 0;

            errorSum = (errorSum + error) << (32 - errorSumBits) >> (32 - errorSumBits); // truncate and sign extend
            filterOut = error * g1 + errorSum * g2;
            number_of_toggles = 0;
        }

        clkCounter = newCounter;
    }
    return output_size;
}
