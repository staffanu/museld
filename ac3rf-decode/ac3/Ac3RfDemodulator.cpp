//
// Created by Staffan Ulfberg on 9/26/25.
//

#include <cassert>
#include <complex>
#include <unistd.h>
#include <format>
#include "../filter/FirPM.h"
#include "../filter/RaisedCosine.h"
#include "../rs/ByteWithErasureFlag.h"
#include "../rs/ReedSolomon.h"
#include "Ac3RfDemodulator.h"

Ac3RfDemodulator::Ac3RfDemodulator(Logger &log, double input_sample_frequency, int input_block_size, bool use_simd)
: m_log(log),
  m_input_sample_frequency(input_sample_frequency),
  m_input_block_size(input_block_size),
  m_dpll(nullptr),
  m_input_framer(log),
  m_block_handler(log)
{
    // The AC3-RF signal uses the band 2.88 MHz +- 150 kHz.  We decimate to have a sample frequency of at least 7 MHz
    // before mixing
    int pre_mix_log2_decimation = floor(::log(m_input_sample_frequency / 7e6) / ::log(2));
    assert(pre_mix_log2_decimation >= 0);
    m_pre_mix_decimation_factor = 1 << pre_mix_log2_decimation;
    double mix_frequency = m_input_sample_frequency / m_pre_mix_decimation_factor;

    // After mixing we make sure to have at least 5 samples per QPSK symbol
    int post_mix_log2_decimation = floor(::log(mix_frequency / 5 / 288e3) / ::log(2));

    m_post_mix_decimation_factor = 1 << post_mix_log2_decimation;
    assert(m_input_block_size % (m_pre_mix_decimation_factor * m_post_mix_decimation_factor) == 0);

    m_dpll = new Ac3DPLL(log, mix_frequency / m_post_mix_decimation_factor,
        m_input_block_size / m_pre_mix_decimation_factor / m_post_mix_decimation_factor);

    // Buffer after low-pass filtering the IQ signal
    m_lp_iq_re_buffer.resize(m_input_block_size / m_pre_mix_decimation_factor / m_post_mix_decimation_factor);
    m_lp_iq_im_buffer.resize(m_input_block_size / m_pre_mix_decimation_factor / m_post_mix_decimation_factor);

    m_pre_mix_filter_stages.resize(pre_mix_log2_decimation);
    m_post_mix_filter_stages.resize(post_mix_log2_decimation + 1); // always add a low-pass filter stage after decimation

    // We create the filter stages in reverse order so that we can set the output buffer of each stage
    // to the stage created previously.
    {
        std::string description = "RRC filter";
        std::vector<double> filter_d = RaisedCosine::rrcFilter(
            mix_frequency / m_post_mix_decimation_factor / 288e3, 3, 0.7);
        std::vector<float> filter(filter_d.cbegin(), filter_d.cend());
        m_post_mix_filter_stages[post_mix_log2_decimation] = new ComplexFirFilterStage(
            "Final lowpass filter",
            description,
            filter,
            1,
            m_input_block_size >> (pre_mix_log2_decimation + post_mix_log2_decimation),
            0,
            &m_lp_iq_re_buffer,
            &m_lp_iq_im_buffer,
            use_simd);
    }

    for (int i = post_mix_log2_decimation - 1; i >= 0; i--) {
        double stage_sample_freq = input_sample_frequency / (1 << (pre_mix_log2_decimation + i));
        // These are all half-band filters, and they could all be the same, since the requirement is very easy to meet,
        // but if designing them this way we do not have to think at all:)
        // Also, some coefficients are zero, so we could use a specialized filter that if more efficient
        auto [description, filter] = FirPM::low_pass<float>(i == post_mix_log2_decimation - 1 ? 22 : 14, stage_sample_freq, 300e3, stage_sample_freq / 2 - 300e3);
        m_post_mix_filter_stages[i] = new ComplexFirFilterStage(
            std::format("Post mix decimate by 2 stage {}", i + 1),
            description,
            filter,
            2,
            m_input_block_size >> (pre_mix_log2_decimation + i),
            m_post_mix_filter_stages[i + 1]->filterSize() - 1,
            m_post_mix_filter_stages[i + 1]->inputReBuffer(),
            m_post_mix_filter_stages[i + 1]->inputImBuffer(),
            use_simd);
    }

    for (int i = pre_mix_log2_decimation - 1; i >= 0; i--) {
        double stage_sample_freq = input_sample_frequency / (1 << i);
        auto [description, filter] = FirPM::low_pass<float>(14, stage_sample_freq, 3.2e6, stage_sample_freq / 2 - 3.2e6);
        m_pre_mix_filter_stages[i] = new FirFilterStage(
            std::format("Pre mix decimate by 2 stage {}", i + 1),
            description,
            filter,
            2,
            m_input_block_size >> i,
            i == pre_mix_log2_decimation - 1 ? m_post_mix_filter_stages[0]->filterSize() - 1 : m_pre_mix_filter_stages[i + 1]->filterSize() - 1,
            i == pre_mix_log2_decimation - 1 ? m_post_mix_filter_stages[0]->inputReBuffer() : m_pre_mix_filter_stages[i + 1]->inputBuffer(),
            use_simd);
    }

    log.debug(eAudio, std::format(
        "Mixing sample frequency={}, sample frequency for reclocking={}, post decimation samples per symbol={:.1f}",
        mix_frequency, mix_frequency / m_post_mix_decimation_factor, mix_frequency / m_post_mix_decimation_factor / 288e3));

    for (const auto &stage: m_pre_mix_filter_stages)
        m_log.debug(eAudio, std::format("Filter stage: {}", stage->toString()));
    for (const auto &stage: m_post_mix_filter_stages)
        m_log.debug(eAudio, std::format("Filter stage: {}", stage->toString()));

    // Create a lookup table for exp(i * 2 * pi * 2.88e6 * t)
    for (int i = 0; i < 1 << c_phase_accum_bits; i++)
        m_exp_lut[i] = std::polar(1.0, 2.0 * M_PI * i / (1 << c_phase_accum_bits));
    double exact_phase_step = (1 << c_phase_accum_bits) * 2.88e6 / mix_frequency;
    m_phase_step = (int)exact_phase_step;
    log.debug(eAudio, std::format("Relative 2.88 MHz frequency error due to integer phase accumulator: {:.2f} %",
        100 * (exact_phase_step - m_phase_step) / exact_phase_step));
    m_phase_accumulator = 0;
}

Ac3RfDemodulator::~Ac3RfDemodulator() {
    for (const auto stage: m_pre_mix_filter_stages)
        delete stage;
    m_pre_mix_filter_stages.clear();
    for (const auto stage: m_post_mix_filter_stages)
        delete stage;
    m_post_mix_filter_stages.clear();

    delete m_dpll;
}

std::vector<uint8_t> Ac3RfDemodulator::demodulateToSymbols(const float *input_buffer) {
    if (m_pre_mix_filter_stages.empty()) {
        // If were not decimating the input before mixing, we store the input directly in the first
        // post-mix filter's real input buffer
        float *first_stage_input_buffer = m_post_mix_filter_stages[0]->inputReBuffer()->data() + m_post_mix_filter_stages[0]->filterSize() - 1;
        for (int i = 0; i < m_input_block_size; i++)
            first_stage_input_buffer[i] = input_buffer[i];
    } else {
        // Store the input in the first filter's input buffer
        float *first_stage_input_buffer = m_pre_mix_filter_stages[0]->inputBuffer()->data() + m_pre_mix_filter_stages[0]->filterSize() - 1;
        for (int i = 0; i < m_input_block_size; i++)
            first_stage_input_buffer[i] = input_buffer[i];

        // Low-pass filter and decimate the input signal
        for (const auto stage: m_pre_mix_filter_stages)
            stage->applyFilter();
    }

    // Mix the input signal with exp(i * 2 * pi * 2.88e6 * t)
    // Notice the last pre-mix filter stores the result in the first post-mix filter's real input buffer
    float *first_stage_input_re = m_post_mix_filter_stages[0]->inputReBuffer()->data() + m_post_mix_filter_stages[0]->filterSize() - 1;
    float *first_stage_input_im = m_post_mix_filter_stages[0]->inputImBuffer()->data() + m_post_mix_filter_stages[0]->filterSize() - 1;
    for (int i = 0; i < m_input_block_size / m_pre_mix_decimation_factor; i++) {
        first_stage_input_im[i] = first_stage_input_re[i] * m_exp_lut[m_phase_accumulator].imag();
        first_stage_input_re[i] = first_stage_input_re[i] * m_exp_lut[m_phase_accumulator].real();
        m_phase_accumulator = (m_phase_accumulator + m_phase_step) & ((1 << c_phase_accum_bits) - 1);
    }

    // Decimate the I/Q signal in stages and finally lowpass
    for (const auto stage: m_post_mix_filter_stages)
        stage->applyFilter();

    // Reclock the incoming data and compute differential QPSK symbols
    auto symbols = m_dpll->reclockSymbols(m_lp_iq_re_buffer, m_lp_iq_im_buffer);

    // Move the last part of the filter inputs back to the beginning of the buffers
    for (auto stage: m_pre_mix_filter_stages)
        stage->moveDataToFront();
    for (auto stage: m_post_mix_filter_stages)
        stage->moveDataToFront();

    return symbols;
}

std::vector<std::array<uint8_t, 1536>> Ac3RfDemodulator::decodeSymbols(const std::vector<uint8_t> &symbols) {
    // Find the sync patterns and break the sequence up into frames (each frame is numbered 0-71 and has 37 bytes of data)
    auto frames = m_input_framer.arrangeInFrames(symbols);

    // For each set of frames 0-71, create a block, error correct it, and parse it for output data
    std::vector<std::array<uint8_t, 1536>> result;
    for (auto frame: frames)
        if (auto block = m_block_handler.handleFrame(frame); block.has_value())
            if (auto correctedBlock = m_block_handler.errorCorrectBlock(block.value()); correctedBlock.has_value()) {
                auto output = m_block_handler.handleCorrectedBlock(correctedBlock.value());
                result.insert(result.end(), output.cbegin(), output.cend());
            }

    return result;
}

std::vector<std::array<uint8_t, 1536>> Ac3RfDemodulator::demodulate(const float *input_buffer) {
    return decodeSymbols(demodulateToSymbols(input_buffer));
}

std::string Ac3RfDemodulator::reedSolomonStatistics() {
    return m_block_handler.reedSolomonStatistics();
}
