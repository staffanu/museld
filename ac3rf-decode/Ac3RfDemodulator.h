//
// Created by Staffan Ulfberg on 9/26/25.
//

#ifndef AC3RF_DECODE_AC3RFDEMODULATOR_H
#define AC3RF_DECODE_AC3RFDEMODULATOR_H

#include <complex>
#include <string>

#include "Ac3BlockHandler.h"
#include "Ac3DPLL.h"
#include "Ac3InputFraming.h"
#include "InputReader.h"
#include "Logger.h"
#include "rs/ReedSolomon.h"

class Ac3RfDemodulator {
public:
    Ac3RfDemodulator(Logger &log, double input_sample_frequency, int input_block_size, int output_fd);
    ~Ac3RfDemodulator();

    Ac3RfDemodulator(const Ac3RfDemodulator &) = delete;
    Ac3RfDemodulator &operator=(const Ac3RfDemodulator &) = delete;
    Ac3RfDemodulator(Ac3RfDemodulator &&) = delete;
    Ac3RfDemodulator &operator=(Ac3RfDemodulator &&) = delete;

    std::vector<std::array<uint8_t, 1536>> demodulate(float *input_buffer);
    std::string reedSolomonStatistics() const;

private:
    static void firFilter(const std::complex<float> *input, size_t input_length,
        const float *filter, size_t filter_length,
        std::complex<float> *output, int decimation_factor);

    // For each sample, compare it to a sample symbol_distance back to determine the current symbol
    static void decodeSymbols(const std::vector<std::complex<float>> &lp_iq_buffer, int symbol_distance,
        std::vector<uint8_t> &symbol_buffer, int buffer_size);

    Logger &m_log;
    double m_input_sample_frequency;
    int m_input_block_size;
    int m_output_fd;

    Ac3DPLL m_dpll;
    Ac3InputFraming m_input_framer;
    Ac3BlockHandler m_block_handler;

    // The signal is first decimated by a factor of 4 repeatedly, until the final decimation is 2 or 4.
    // At that time, the final lowpass filter is applied.
    constexpr static int max_filter_stages = 4;
    int m_decimation_factor;
    struct FilterStage {
        FilterStage(int factor, std::vector<float> filter, int input_buffer_size_without_overlap, int output_offset) {
            decimation_factor = factor;
            this->filter = filter;
            this->input_buffer_size_without_overlap = input_buffer_size_without_overlap;
            this->output_offset = output_offset;
            input_buffer.resize(input_buffer_size_without_overlap + filter.size());
        }
        int decimation_factor;
        int input_buffer_size_without_overlap;
        int output_offset;
        std::vector<float> filter;
        std::vector<std::complex<float>> input_buffer;
        std::vector<std::complex<float>> *output_buffer; // pointer to next
    };
    std::vector<FilterStage> m_filter_stages;
    std::vector<std::complex<float>> m_lp_iq_buffer; // Buffer after low-pass filtering the IQ signal -- overlap to be able to look back

    int m_symbol_distance; // This is the number of samples apart that two subsequent symbols appear in the decimated input.

    constexpr static int c_phase_accum_bits = 10;
    std::array<std::complex<float>, 1 << c_phase_accum_bits> m_exp_lut;
    int m_phase_step;
    int m_phase_accumulator;

    // For each sample, we compare it to a sample symbol_distance back to determine the current symbol
    std::vector<uint8_t> m_symbol_buffer; // Raw symbols at sample_frequency / decimation Hz
    int m_max_number_of_reclocked_symbols;
    std::vector<uint8_t> m_reclocked_symbol_buffer; // Symbols after DPLL -- rate is approximately sample_frequency / decimation / symbol_distance
};

#endif //AC3RF_DECODE_AC3RFDEMODULATOR_H
