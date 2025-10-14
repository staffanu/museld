//
// Created by Staffan Ulfberg on 10/14/25.
//

#include "FirFilterStage.h"

#include <numeric>
#include <ranges>
#include <gnuradio/filter/firdes.h>

#include "Ac3RfDemodulator.h"

FirFilterStage::FirFilterStage(
    std::string name,
    double sample_frequency,
    double cutoff_frequency,
    double transition_width,
    int decimation_factor,
    int input_buffer_size_without_overlap,
    int output_offset,
    std::vector<float> *output_re_buffer,
    std::vector<float> *output_im_buffer)
  : m_name(name),
    m_sample_frequency(sample_frequency),
    m_cutoff_frequency(cutoff_frequency),
    m_transition_width(transition_width),
    m_filter(gr::filter::firdes::low_pass(1.0, sample_frequency, cutoff_frequency, transition_width, gr::fft::window::WIN_HAMMING)),
    m_decimation_factor(decimation_factor),
    m_input_buffer_size_without_overlap(input_buffer_size_without_overlap),
    m_output_offset(output_offset),
    m_input_re_buffer(new std::vector<float>(input_buffer_size_without_overlap + m_filter.size() - 1)),
    m_input_im_buffer(new std::vector<float>(input_buffer_size_without_overlap + m_filter.size() - 1)),
    m_output_re_buffer(output_re_buffer),
    m_output_im_buffer(output_im_buffer) {

    std::reverse(m_filter.begin(), m_filter.end());

    for (int i = 0; i < m_filter.size(); i++)
        printf("%f ", m_filter[i]);
    printf("\n");
}

FirFilterStage::~FirFilterStage() {
    delete m_input_re_buffer;
    delete m_input_im_buffer;
}

std::string FirFilterStage::name() const {
    return m_name;
}

int FirFilterStage::filterSize() const {
    return m_filter.size();
}

int FirFilterStage::decimationFactor() const {
    return m_decimation_factor;
}

std::vector<float> *FirFilterStage::inputReBuffer() {
    return m_input_re_buffer;
}
std::vector<float> *FirFilterStage::inputImBuffer() {
    return m_input_im_buffer;
}

std::string FirFilterStage::toString() const {
    return std::format("{}, sample_freq: {}, cutoff: {}, trans w: {}, taps: {}, decimation: {}",
        m_name, m_sample_frequency, m_cutoff_frequency, m_transition_width, m_filter.size(), m_decimation_factor);
}

void FirFilterStage::setInput(int index, std::complex<float> v) {
    (*m_input_re_buffer)[index] = v.real();
    (*m_input_im_buffer)[index] = v.imag();
}

void FirFilterStage::applyFilter() {
    firFilter(m_input_re_buffer->data(), m_input_buffer_size_without_overlap, m_filter.data(),
    m_filter.size(), m_output_re_buffer->data() + m_output_offset, m_decimation_factor);
    firFilter(m_input_im_buffer->data(), m_input_buffer_size_without_overlap, m_filter.data(),
    m_filter.size(), m_output_im_buffer->data() + m_output_offset, m_decimation_factor);
}

void FirFilterStage::moveDataToFront() {
    memmove(m_input_re_buffer->data(), m_input_re_buffer->data() + m_input_buffer_size_without_overlap, (m_filter.size() - 1) * sizeof(float));
    memmove(m_input_im_buffer->data(), m_input_im_buffer->data() + m_input_buffer_size_without_overlap, (m_filter.size() - 1) * sizeof(float));
}

#ifdef __AVX__
#include <immintrin.h>
#include <numeric>

// Filters the real or imaginary part, so uses every other float of the input/output
void FirFilterStage::firFilter(
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

void FirFilterStage::firFilter(
        const float *input,  // input signal of length output_length + filter_length - 1
        size_t input_length, // input signal size, excluding filter_length-1 extra values at end
        const float *filter, // reversed filter coefficients
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
