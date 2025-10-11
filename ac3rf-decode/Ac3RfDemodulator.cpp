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
    m_lp_iq_re_buffer.resize(m_input_block_size / m_decimation_factor + m_symbol_distance);
    m_lp_iq_im_buffer.resize(m_input_block_size / m_decimation_factor + m_symbol_distance);

    for (int i = 0; i < m_filter_stages.size(); i++) {
        m_filter_stages[i].output_re_buffer = i != m_filter_stages.size() - 1 ? &m_filter_stages[i + 1].input_re_buffer : &m_lp_iq_re_buffer;
        m_filter_stages[i].output_im_buffer = i != m_filter_stages.size() - 1 ? &m_filter_stages[i + 1].input_im_buffer : &m_lp_iq_im_buffer;
    }

    for (int i = 0; i < 1 << c_phase_accum_bits; i++)
        m_exp_lut[i] = std::polar(1.0, 2.0 * M_PI * i / (1 << c_phase_accum_bits));
    m_phase_step = (1 << c_phase_accum_bits) * 2.88e6 / m_input_sample_frequency;
    m_phase_accumulator = 0;

    m_max_number_of_reclocked_symbols = m_input_block_size / m_decimation_factor / m_symbol_distance * 17 / 16;
    m_reclocked_symbol_buffer.resize(m_max_number_of_reclocked_symbols);
}

Ac3RfDemodulator::~Ac3RfDemodulator() {
}

std::vector<std::array<uint8_t, 1536>> Ac3RfDemodulator::demodulate(float *input_buffer) {
    // Mix the input signal with exp(i * 2 * pi * 288e6 * t)
    for (int i = 0; i < m_input_block_size; i++) {
        m_filter_stages[0].input_re_buffer[i + m_filter_stages[0].filter.size()] = input_buffer[i] * m_exp_lut[m_phase_accumulator].real();
        m_filter_stages[0].input_im_buffer[i + m_filter_stages[0].filter.size()] = input_buffer[i] * m_exp_lut[m_phase_accumulator].imag();
        m_phase_accumulator = (m_phase_accumulator + m_phase_step) & ((1 << c_phase_accum_bits) - 1);
    }

    // Decimate the I/Q signal in stages and finally lowpass the I/Q signal
    for (int i = 0; i < m_filter_stages.size(); i++) {
        firFilter(m_filter_stages[i].input_re_buffer.data(), m_filter_stages[i].input_buffer_size_without_overlap, m_filter_stages[i].filter.data(),
            m_filter_stages[i].filter.size(), m_filter_stages[i].output_re_buffer->data() + m_filter_stages[i].output_offset, m_filter_stages[i].decimation_factor);
        firFilter(m_filter_stages[i].input_im_buffer.data(), m_filter_stages[i].input_buffer_size_without_overlap, m_filter_stages[i].filter.data(),
        m_filter_stages[i].filter.size(), m_filter_stages[i].output_im_buffer->data() + m_filter_stages[i].output_offset, m_filter_stages[i].decimation_factor);
    }

    // Create a "raw" symbol stream from the phase difference of the baseband data 1/288e3 seconds apart
    decodeSymbols(m_lp_iq_re_buffer, m_lp_iq_im_buffer, m_symbol_distance, m_symbol_buffer, m_input_block_size / m_decimation_factor);

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
    for (int i = 0; i < m_filter_stages.size(); i++) {
        memmove(m_filter_stages[i].input_re_buffer.data(), m_filter_stages[i].input_re_buffer.data() + m_filter_stages[i].input_buffer_size_without_overlap, m_filter_stages[i].filter.size() * sizeof(std::complex<float>));
        memmove(m_filter_stages[i].input_im_buffer.data(), m_filter_stages[i].input_im_buffer.data() + m_filter_stages[i].input_buffer_size_without_overlap, m_filter_stages[i].filter.size() * sizeof(std::complex<float>));
    }
    memmove(m_lp_iq_re_buffer.data(), m_lp_iq_re_buffer.data() + m_lp_iq_re_buffer.size() - m_symbol_distance, m_symbol_distance * sizeof(std::complex<float>));
    memmove(m_lp_iq_im_buffer.data(), m_lp_iq_im_buffer.data() + m_lp_iq_im_buffer.size() - m_symbol_distance, m_symbol_distance * sizeof(std::complex<float>));

    return result;
}

std::string Ac3RfDemodulator::reedSolomonStatistics() const {
    return m_block_handler.reedSolomonStatistics();
}

#ifdef __AVX__
#include <immintrin.h>
#include <numeric>

// Filters the real or imaginary part, so uses every other float of the input/output
void Ac3RfDemodulator::firFilter(
    const float *input,   // input signal of length output_length + filter_length - 1 -- stored in every other float
    size_t input_length,  // usable input (not including the filter_length-1 extra values)
    const float *filter,  // reversed filter coefficients
    size_t filter_length,
    float *output,        // the output size is input_length / decimation_factor, again stored in every other float
    int decimation_factor) {

    assert(input_length % decimation_factor == 0);
    const int output_size = input_length / decimation_factor;
    assert(output_size % 2 == 0); // even output length!
    assert(c_AVX_floats_per_chunk * sizeof(float) == 32);
    assert(((long)filter) % 32 == 0);

    alignas(__m256) std::array<float, c_AVX_floats_per_chunk> tmp_store0{};
    alignas(__m256) std::array<float, c_AVX_floats_per_chunk> tmp_store1{};

    for (int oi = 0, ii = 0; oi < output_size; oi += 2, ii += 2 * decimation_factor) {
        __m256 out_chunk0 = _mm256_setzero_ps();
        __m256 out_chunk1 = _mm256_setzero_ps();

        for (int j = 0; j < filter_length; j += c_AVX_floats_per_chunk) {
            __m256 filter_chunk = _mm256_loadu_ps(filter + j); // TODO: change to load_ps and require filter to be aligned

            __m256 input_chunk0 = _mm256_loadu_ps(input + ii + j);
            out_chunk0 = _mm256_add_ps(out_chunk0, _mm256_mul_ps(input_chunk0, filter_chunk));

            __m256 input_chunk1 = _mm256_loadu_ps(input + ii + decimation_factor + j);
            out_chunk1 = _mm256_add_ps(out_chunk1, _mm256_mul_ps(input_chunk1, filter_chunk));
        }
        _mm256_store_ps(tmp_store0.data(), out_chunk0); // aligned store
        output[oi] = std::accumulate(tmp_store0.begin(), tmp_store0.end(), 0.f);

        _mm256_store_ps(tmp_store1.data(), out_chunk1); // aligned store
        output[oi + 1] = std::accumulate(tmp_store1.begin(), tmp_store1.end(), 0.f);
    }
}

#else
#warning "AVX not detected"

void Ac3RfDemodulator::firFilter(
        const float *input,   // input signal of length output_length + filter_length - 1
        size_t input_length, // usable input (not including the filter_length-1 extra values)
        const float *filter,  // reversed filter coefficients
        size_t filter_length,
        float *output,
        int decimation_factor) {
    for (auto i = 0; i < input_length / decimation_factor; i++) {
        float s = 0;
        for (auto j = 0; j < filter_length; j++) {
            s += input[i * decimation_factor + j] * filter[j];
        }
        output[i] = s;
    }
}
#endif

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
