// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_AC3RFDEMODULATOR_H
#define AC3RF_DECODE_AC3RFDEMODULATOR_H

#include <complex>
#include <string>
#include "../Logger.h"
#include "../filter/ComplexFirFilterStage.h"
#include "Ac3DPLL.h"

/*
 * Analog processing for AC3-RF demodulation: down-sampling, filtering, mixing,
 * and timing recovery.  The output is a stream of raw QPSK symbols (two bits used
 * in every output byte).
 * Pass the result to Ac3Decoder::decodeSymbols() for framing + RS decoding.
 */
class Ac3RfDemodulator {
public:
    Ac3RfDemodulator(Logger &log, double input_sample_frequency, int input_block_size, bool use_simd);
    ~Ac3RfDemodulator();

    Ac3RfDemodulator(const Ac3RfDemodulator &) = delete;
    Ac3RfDemodulator &operator=(const Ac3RfDemodulator &) = delete;
    Ac3RfDemodulator(Ac3RfDemodulator &&) = delete;
    Ac3RfDemodulator &operator=(Ac3RfDemodulator &&) = delete;

    // Since the input is down-sampled and run through filters using SIMD
    // demodulateToSymbols needs input that is a multiple of the number returned by this method.
    [[nodiscard]] int inputSampleAlignment() const;

    // n_samples must be a multiple of inputSampleAlignment() and
    // at most the block size passed at construction.
    std::vector<uint8_t> demodulateToSymbols(const float *input_buffer, size_t n_samples);

private:
    Logger &m_log;
    double m_input_sample_frequency;
    int m_input_block_size;

    Ac3DPLL *m_dpll;

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
