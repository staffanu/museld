//
// Created by Staffan Ulfberg on 9/26/25.
//

#include <cassert>
#include <complex>
#include <unistd.h>
#include <cstring>
#include <format>
#include "../filter/FirPM.h"
#include "../rs/ByteWithErasureFlag.h"
#include "../rs/ReedSolomon.h"
#include "Ac3RfDemodulator.h"

Ac3RfDemodulator::Ac3RfDemodulator(Logger &log, double input_sample_frequency, int input_block_size, int output_fd, bool use_simd)
: m_log(log),
  m_input_sample_frequency(input_sample_frequency),
  m_input_block_size(input_block_size),
  m_output_fd(output_fd),
  m_dpll(log),
  m_input_framer(log),
  m_block_handler(log)
{
    // After decimation, we make sure to have at least 5 samples per QPSK symbol, but also ensure that
    // decimation is a power of two (it is sufficient that it divides block_size, which is probably also a power of two).
    int log2decimation = floor(::log(m_input_sample_frequency / 5 / 288e3) / ::log(2));
    m_decimation_factor = 1 << log2decimation;
    assert(m_input_block_size % m_decimation_factor == 0);

    int decimations_by_4 = log2decimation / 2;
    assert(decimations_by_4 >= 1); // True for sample rates >= 11.52 MHz
    double final_input_frequency = m_input_sample_frequency / (1 << (2 * decimations_by_4));

    // This is the number of samples apart that two adjacent symbols appear in the decimated input.
    m_symbol_distance = round(m_input_sample_frequency / 288e3 / m_decimation_factor);

    // The first filter's input buffer is the input multiplied by exp(i * 2 * pi * 2.88e6),
    // the last filter's output is set to m_lp_iq_buffer

    // Buffer after low-pass filtering the IQ signal -- overlap to be able to look back
    m_lp_iq_re_buffer.resize(m_input_block_size / m_decimation_factor + m_symbol_distance);
    m_lp_iq_im_buffer.resize(m_input_block_size / m_decimation_factor + m_symbol_distance);

    // We create the filter stages in reverse order, so that we can set the output buffer of each stage
    // to the stage created previously.
    m_filter_stages.resize(decimations_by_4 + 1);

    {
        auto [description, filter] = FirPM::low_pass<float>(31,  final_input_frequency, 150e3, 300e3);
        m_filter_stages[decimations_by_4] = new ComplexFirFilterStage(
            "Final lowpass filter",
            description,
            filter,
            m_decimation_factor >> (2 * decimations_by_4),
            m_input_block_size >> (2 * decimations_by_4),
            m_symbol_distance,
            &m_lp_iq_re_buffer,
            &m_lp_iq_im_buffer,
            use_simd);
    }

    for (int i = decimations_by_4 - 1; i >= 0; i--) {
        double stage_sample_freq = input_sample_frequency / (1 << (i * 2));
        auto [description, filter] = FirPM::low_pass<float>(23, stage_sample_freq, 150e3, stage_sample_freq / 4 - 150e3);
        m_filter_stages[i] = new ComplexFirFilterStage(
            std::format("Decimate by 4 stage {}", i + 1),
            description,
            filter,
            4, m_input_block_size >> (2 * i),
            m_filter_stages[i + 1]->filterSize() - 1,
            m_filter_stages[i + 1]->inputReBuffer(),
            m_filter_stages[i + 1]->inputImBuffer(),
            use_simd);
    }

    for (const auto &stage: m_filter_stages)
        m_log.debug(eAudio, std::format("Filter stage: {}", stage->toString()));

    // Create a lookup table for exp(i * 2 * pi * 2.88e6)
    for (int i = 0; i < 1 << c_phase_accum_bits; i++)
        m_exp_lut[i] = std::polar(1.0, 2.0 * M_PI * i / (1 << c_phase_accum_bits));
    double exact_phase_step = (1 << c_phase_accum_bits) * 2.88e6 / m_input_sample_frequency;
    m_phase_step = (int)exact_phase_step;
    log.debug(eAudio, std::format("Relative 2.88 MHz frequency error due to integer phase accumulator: {:.2f} %",
        100 * (exact_phase_step - m_phase_step) / exact_phase_step));
    m_phase_accumulator = 0;

    // The number of symbols after the PLL will not be exactly the same each time. Allow for extra.
    m_max_number_of_reclocked_symbols = m_input_block_size / m_decimation_factor / m_symbol_distance * 17 / 16 + 10;
    m_reclocked_symbol_buffer.resize(m_max_number_of_reclocked_symbols);
}

Ac3RfDemodulator::~Ac3RfDemodulator() {
    for (const auto stage: m_filter_stages)
        delete stage;
    m_filter_stages.clear();
}

std::vector<std::array<uint8_t, 1536>> Ac3RfDemodulator::demodulate(float *input_buffer) {
    // Mix the input signal with exp(i * 2 * pi * 2.88e6 * t)
    float *first_stage_input_re = m_filter_stages[0]->inputReBuffer()->data() + m_filter_stages[0]->filterSize() - 1;
    float *first_stage_input_im = m_filter_stages[0]->inputImBuffer()->data() + m_filter_stages[0]->filterSize() - 1;
    for (int i = 0; i < m_input_block_size; i++) {
        first_stage_input_re[i] = input_buffer[i] * m_exp_lut[m_phase_accumulator].real();
        first_stage_input_im[i] = input_buffer[i] * m_exp_lut[m_phase_accumulator].imag();
        m_phase_accumulator = (m_phase_accumulator + m_phase_step) & ((1 << c_phase_accum_bits) - 1);
    }

    // Decimate the I/Q signal in stages and finally lowpass
    for (auto stage: m_filter_stages)
        stage->applyFilter();

    // Create a "raw" symbol stream from the phase difference of the baseband data 1/288e3 seconds apart
    decodeSymbols(m_lp_iq_re_buffer, m_lp_iq_im_buffer, m_symbol_distance, m_symbol_buffer, m_input_block_size / m_decimation_factor);

    // Reclock the symbol changes using a DPLL -- the actual number of output symbols can vary slightly
    int symbol_count = m_dpll.reclockSymbols(
        m_input_sample_frequency / m_decimation_factor,
        m_symbol_buffer, m_input_block_size / m_decimation_factor,
        m_reclocked_symbol_buffer, m_max_number_of_reclocked_symbols);

    // Find the sync pattens and break the sequence up into frames (each frame is numbered 0-71 and has 37 bytes of data)
    auto frames = m_input_framer.arrangeInFrames(m_reclocked_symbol_buffer, symbol_count);

    // For each set of frames 0-71, create a block, error correct it, and parse it for output data
    std::vector<std::array<uint8_t, 1536>> result;
    for (auto frame: frames)
        if (auto block = m_block_handler.handleFrame(frame); block.has_value())
            if (auto correctedBlock = m_block_handler.errorCorrectBlock(block.value()); correctedBlock.has_value()) {
                auto output = m_block_handler.handleCorrectedBlock(correctedBlock.value());
                result.insert(result.end(), output.cbegin(), output.cend());
            }

    // Move the last part of the filter inputs back to the beginning of the buffers
    for (auto stage: m_filter_stages)
        stage->moveDataToFront();

    // Move the last part of the lp filter output back to the beginning
    memmove(m_lp_iq_re_buffer.data(), m_lp_iq_re_buffer.data() + m_lp_iq_re_buffer.size() - m_symbol_distance, m_symbol_distance * sizeof(float));
    memmove(m_lp_iq_im_buffer.data(), m_lp_iq_im_buffer.data() + m_lp_iq_im_buffer.size() - m_symbol_distance, m_symbol_distance * sizeof(float));

    return result;
}

std::string Ac3RfDemodulator::reedSolomonStatistics() {
    return m_block_handler.reedSolomonStatistics();
}

void Ac3RfDemodulator::decodeSymbols(const std::vector<float> &lp_iq_re_buffer, const std::vector<float> &lp_iq_im_buffer,
    int symbol_distance, std::vector<uint8_t> &symbol_buffer, int buffer_size) {

    auto symbol_decoder = [](std::complex<float> prev, std::complex<float> current) -> uint8_t {
        auto quad = current * conj(prev);
        bool isTopLeft = quad.imag() > quad.real();
        bool isTopRight = quad.imag() > -quad.real();
        if (!isTopLeft && !isTopRight)
            return 2;
        if (!isTopLeft && isTopRight)
            return 0;
        if (isTopLeft && !isTopRight)
            return 3;
        if (isTopLeft && isTopRight)
            return 1;
        throw std::runtime_error("Invalid quadrant");
    };

    symbol_buffer.resize(buffer_size);
    for (int i = 0; i < buffer_size; i++) {
        symbol_buffer[i] = symbol_decoder(std::complex<float>(lp_iq_re_buffer[i], lp_iq_im_buffer[i]),
            std::complex(lp_iq_re_buffer[i + symbol_distance], lp_iq_im_buffer[i + symbol_distance]));
    }
}
