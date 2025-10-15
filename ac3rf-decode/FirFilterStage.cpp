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
    std::vector<float> *output_im_buffer,
    bool use_simd)
  : m_name(name),
    m_sample_frequency(sample_frequency),
    m_cutoff_frequency(cutoff_frequency),
    m_transition_width(transition_width),
    m_filter(gr::filter::firdes::low_pass(1.0, sample_frequency, cutoff_frequency, transition_width, gr::fft::window::WIN_HAMMING)),
    m_decimation_factor(decimation_factor),
    m_input_buffer_size_without_overlap(input_buffer_size_without_overlap),
    m_output_offset(output_offset),
    m_output_re_buffer(output_re_buffer),
    m_output_im_buffer(output_im_buffer),
    m_use_simd(use_simd) {

    std::reverse(m_filter.begin(), m_filter.end());
    if (m_use_simd) {
        // resize filter so length is multiple of chunk size
        m_filter.resize((m_filter.size() * c_AVX_floats_per_chunk + c_AVX_floats_per_chunk) / c_AVX_floats_per_chunk);
    }
    m_input_re_buffer = new std::vector<float>(input_buffer_size_without_overlap + m_filter.size() - 1);
    m_input_im_buffer = new std::vector<float>(input_buffer_size_without_overlap + m_filter.size() - 1);
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

void FirFilterStage::firFilter(
        const float *input,  // input signal of length output_length + filter_length - 1
        size_t input_length, // input signal size, excluding filter_length-1 extra values at end
        const float *filter, // reversed filter coefficients
        size_t filter_length,
        float *output,
        int decimation_factor) {
    if (m_use_simd) {
#if defined __AVX__
        firFilterAvx(input, input_length, filter, filter_length, output, decimation_factor);
#elif __ARM_NEON == 1
        firFilterNeon(input, input_length, filter, filter_length, output, decimation_factor);
#else
        throw new std::runtime_error("SIMD not implemented");
#endif
    } else {
        firFilterNormal(input, input_length, filter, filter_length, output, decimation_factor);
    }
}

void FirFilterStage::firFilterNormal(
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


#ifdef __AVX__
#include <immintrin.h>
#include <numeric>
#endif

// Filters the real or imaginary part, so uses every other float of the input/output
void FirFilterStage::firFilterAvx(
    const float *input,   // input signal of length output_length + filter_length - 1 -- stored in every other float
    size_t input_length,  // usable input (not including the filter_length-1 extra values)
    const float *filter,  // reversed filter coefficients
    size_t filter_length,
    float *output,        // the output size is input_length / decimation_factor, again stored in every other float
    int decimation_factor) {
#ifdef __AVX__
    assert(input_length % decimation_factor == 0);
    const int output_size = input_length / decimation_factor;
    assert(output_size % 2 == 0); // even output length!
    assert(c_AVX_floats_per_chunk * sizeof(float) == 32);
    assert(input_length % c_AVX_floats_per_chunk == 0);
    assert(filter_length % c_AVX_floats_per_chunk == 0);

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
#endif
}

#if __ARM_NEON == 1
#include <arm_neon.h>
#endif

void FirFilterStage::firFilterNeon(
        const float *input,   // input signal of length at least output_size + filter_length - 1
        size_t input_length,
        const float *filter,  // reversed filter coefficients
        size_t filter_length,
        float *output,        // the output size is input_length / decimation_factor, again stored in every other float
        int decimation_factor) {
#if __ARM_NEON == 1
    const int output_size = input_length / decimation_factor;
    assert(output_size % 2 == 0);
    assert(input_length % c_NEON_floats_per_chunk == 0);
    assert(filter_length % c_NEON_floats_per_chunk == 0);

    alignas(float32x4_t) float tmp_store0[c_NEON_floats_per_chunk];
    alignas(float32x4_t) float tmp_store1[c_NEON_floats_per_chunk];

    for (int oi = 0, ii = 0; oi < output_size; oi += 2, ii += 2 * decimation_factor) {
        float32x4_t out_chunk0 = vdupq_n_f32(0);
        float32x4_t out_chunk1 = vdupq_n_f32(0);

        for (int j = 0; j < filter_length; j += c_NEON_floats_per_chunk) {
            float32x4_t filter_chunk = vld1q_f32(filter + j);

            float32x4_t input_chunk0 = vld1q_f32(input + ii + j);
            out_chunk0 = vmlaq_f32(out_chunk0, input_chunk0, filter_chunk);

            float32x4_t input_chunk1 = vld1q_f32(input + ii + decimation_factor + j);
            out_chunk1 = vmlaq_f32(out_chunk1, input_chunk1, filter_chunk);
        }

        output[oi] = vaddvq_f32(out_chunk0);
        output[oi + 1] = vaddvq_f32(out_chunk1);
    }
#endif
}
