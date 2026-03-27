//
// Created by Staffan Ulfberg on 9/26/25.
//

#ifndef AC3RF_DECODE_AC3RFDEMODULATOR_H
#define AC3RF_DECODE_AC3RFDEMODULATOR_H

#include <complex>
#include <string>
#include "../Logger.h"
#include "../rs/ReedSolomon.h"
#include "../filter/ComplexFirFilterStage.h"
#include "Ac3BlockHandler.h"
#include "Ac3DPLL.h"
#include "Ac3InputFraming.h"

class Ac3RfDemodulator {
public:
    Ac3RfDemodulator(Logger &log, double input_sample_frequency, int input_block_size, bool use_simd);
    ~Ac3RfDemodulator();

    Ac3RfDemodulator(const Ac3RfDemodulator &) = delete;
    Ac3RfDemodulator &operator=(const Ac3RfDemodulator &) = delete;
    Ac3RfDemodulator(Ac3RfDemodulator &&) = delete;
    Ac3RfDemodulator &operator=(Ac3RfDemodulator &&) = delete;

    // Two-stage API: analog chain (filtering + DPLL) and digital chain (framing + RS) separately.
    // The combined demodulate() is equivalent to decodeSymbols(demodulateToSymbols(input_buffer)).
    std::vector<uint8_t> demodulateToSymbols(const float *input_buffer);
    std::vector<std::array<uint8_t, 1536>> decodeSymbols(const std::vector<uint8_t> &symbols);
    std::vector<std::array<uint8_t, 1536>> demodulate(const float *input_buffer);

    std::string reedSolomonStatistics();

private:
    Logger &m_log;
    double m_input_sample_frequency;
    int m_input_block_size;

    Ac3DPLL *m_dpll;
    Ac3InputFraming m_input_framer;
    Ac3BlockHandler m_block_handler;

    // The signal is first decimated by a factor of 4 repeatedly, until the final decimation is 2 or 4.
    // At that time, the final lowpass filter is applied.
    int m_pre_mix_decimation_factor;
    int m_post_mix_decimation_factor;

    std::vector<FirFilterStage *> m_pre_mix_filter_stages;
    std::vector<ComplexFirFilterStage *> m_post_mix_filter_stages;
    std::vector<float> m_lp_iq_re_buffer; // Buffer after low-pass filtering the IQ signal
    std::vector<float> m_lp_iq_im_buffer;

    constexpr static int c_phase_accum_bits = 14;
    std::array<std::complex<float>, 1 << c_phase_accum_bits> m_exp_lut;
    int m_phase_step;
    int m_phase_accumulator;
};

#endif //AC3RF_DECODE_AC3RFDEMODULATOR_H
