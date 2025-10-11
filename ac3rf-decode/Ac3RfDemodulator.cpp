//
// Created by Staffan Ulfberg on 9/26/25.
//

#include <assert.h>
#include <complex>
#include <unistd.h>
#include <gnuradio/filter/firdes.h>
#include "Ac3RfDemodulator.h"
#include "rs/ByteWithErasureFlag.h"
#include "rs/ReedSolomon.h"

#include <format>

Ac3RfDemodulator::Ac3RfDemodulator(Logger &log, double input_sample_frequency, int input_block_size, int output_fd)
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

    auto decimate_by_4_filter = gr::filter::firdes::low_pass_2(1.0, 1.0, 1.0/16, 1.0/8, 40.0, gr::fft::window::WIN_HAMMING);
    std::reverse(decimate_by_4_filter.begin(), decimate_by_4_filter.end());

    double final_input_frequency = m_input_sample_frequency / (1 << (2 * decimations_by_4));
    auto final_lowpass_filter = gr::filter::firdes::low_pass_2(1.0, final_input_frequency, 144e3, 100e3, 40.0, gr::fft::window::WIN_HAMMING);
    std::reverse(final_lowpass_filter.begin(), final_lowpass_filter.end());

    // This is the number of samples apart that two adjacent symbols appear in the decimated input.
    m_symbol_distance = round(m_input_sample_frequency / 288e3 / m_decimation_factor);

    // The first filter's input buffer is the input multiplied by exp(i * 2 * pi * 2.88e6),
    // the last filter's output is set to m_lp_iq_buffer
    for (int i = 0; i < decimations_by_4; i++)
        m_filter_stages.push_back(
            FilterStage(4, decimate_by_4_filter, m_input_block_size >> (2 * i),
                i < decimations_by_4 - 1 ? decimate_by_4_filter.size() : final_lowpass_filter.size()));
    m_filter_stages.push_back(FilterStage(m_decimation_factor >> (2 * decimations_by_4), final_lowpass_filter, m_input_block_size >> (2 * decimations_by_4), m_symbol_distance));

    m_log.debug(std::format("Filter sizes: decim by 4: {}, final lowpass: {}, total filter stages: {}, total decimation: {}",
        decimate_by_4_filter.size(), final_lowpass_filter.size(), m_filter_stages.size(), m_decimation_factor));

    // Buffer after low-pass filtering the IQ signal -- overlap to be able to look back
    m_lp_iq_buffer.resize(m_input_block_size / m_decimation_factor + m_symbol_distance);

    for (int i = 0; i < m_filter_stages.size(); i++)
        m_filter_stages[i].output_buffer = i != m_filter_stages.size() - 1 ? &m_filter_stages[i + 1].input_buffer : &m_lp_iq_buffer;

    m_phase_step = 2.88e6 * 2 * M_PI / m_input_sample_frequency;
    m_phase_accumulator = 0;

    m_max_number_of_reclocked_symbols = m_input_block_size / m_decimation_factor / m_symbol_distance * 17 / 16;
    m_reclocked_symbol_buffer.resize(m_max_number_of_reclocked_symbols);
}

Ac3RfDemodulator::~Ac3RfDemodulator() {
}

std::vector<std::array<uint8_t, 1536>> Ac3RfDemodulator::demodulate(float *input_buffer) {
    // Mix the input signal with exp(i * 2 * pi * 288e6 * t)
    for (int i = 0; i < m_input_block_size; i++) {
        m_filter_stages[0].input_buffer[i + m_filter_stages[0].filter.size()] = input_buffer[i] * std::polar(1.0f, m_phase_accumulator);
        m_phase_accumulator = m_phase_accumulator + m_phase_step >= 2 * M_PI ? m_phase_accumulator + m_phase_step - 2 * M_PI : m_phase_accumulator + m_phase_step;
    }

    // Decimate the I/Q signal in stages and finally lowpass the I/Q signal
    for (int i = 0; i < m_filter_stages.size(); i++) {
        firFilter(m_filter_stages[i].input_buffer.data(), m_filter_stages[i].input_buffer_size_without_overlap, m_filter_stages[i].filter.data(),
            m_filter_stages[i].filter.size(), m_filter_stages[i].output_buffer->data() + m_filter_stages[i].output_offset, m_filter_stages[i].decimation_factor);
    }

    // Create a "raw" symbol stream from the phase difference of the baseband data 1/288e3 seconds apart
    decodeSymbols(m_lp_iq_buffer, m_symbol_distance, m_symbol_buffer, m_input_block_size / m_decimation_factor);

    // Reclock the symbol changes using a DPLL -- the actual number of symbols can vary slightly
    int symbol_count = m_dpll.reclockSymbols(m_input_sample_frequency / m_decimation_factor, m_symbol_buffer, m_input_block_size / m_decimation_factor, m_reclocked_symbol_buffer, m_max_number_of_reclocked_symbols);

    std::vector<std::array<uint8_t, 1536>> result;

    // Find the sync pattens and break the sequence up into frames (each frame is numbered 0-71 and has 37 bytes of data)
    auto frames = m_input_framer.arrangeInFrames(m_reclocked_symbol_buffer, symbol_count);
    for (auto frame: frames)
        if (auto block = m_block_handler.handleFrame(frame.first, frame.second); block.has_value())
            if (auto correctedBlock = m_block_handler.errorCorrectBlock(block.value()); correctedBlock.has_value()) {
                auto output = m_block_handler.handleCorrectedBlock(correctedBlock.value());
                result.insert(result.end(), output.cbegin(), output.cend());
            }

    // Move the last part of the mixed signal and the low-pass filtered signals back to the beginning of the buffers:
    // we need the overlap since the FIR filter, and the symbol decoding, both span a number of samples.
    for (int i = 0; i < m_filter_stages.size(); i++)
        memmove(m_filter_stages[i].input_buffer.data(), m_filter_stages[i].input_buffer.data() + m_filter_stages[i].input_buffer_size_without_overlap, m_filter_stages[i].filter.size() * sizeof(std::complex<float>));
    memmove(m_lp_iq_buffer.data(), m_lp_iq_buffer.data() + m_lp_iq_buffer.size() - m_symbol_distance, m_symbol_distance * sizeof(std::complex<float>));

    return result;
}

std::string Ac3RfDemodulator::reedSolomonStatistics() const {
    return m_block_handler.reedSolomonStatistics();
}

void Ac3RfDemodulator::firFilter(
        const std::complex<float> *input,   // input signal of length output_length + filter_length - 1
        size_t input_length, // usable input (not including the filter_length-1 extra values)
        const float *filter,  // reversed filter coefficients
        size_t filter_length,
        std::complex<float> *output,
        int decimation_factor) {
    for (auto i = 0; i < input_length / decimation_factor; i ++) {
        std::complex<double> s = 0;
        for (auto j = 0; j < filter_length; j++) {
            s += input[i * decimation_factor + j] * filter[j];
        }
        output[i] = s;
    }
}

void Ac3RfDemodulator::decodeSymbols(const std::vector<std::complex<float>> &lp_iq_buffer, int symbol_distance,
    std::vector<uint8_t> &symbol_buffer, int buffer_size) {

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
        symbol_buffer[i] = symbol_decoder(lp_iq_buffer[i], lp_iq_buffer[i + symbol_distance]);
    }
}
