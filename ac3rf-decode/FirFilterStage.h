//
// Created by Staffan Ulfberg on 10/14/25.
//

#ifndef AC3RF_DECODE_FIRFILTERSTAGE_H
#define AC3RF_DECODE_FIRFILTERSTAGE_H

#include <memory>
#include <vector>

class FirFilterStage {
public:
    FirFilterStage(std::string name,
        double sample_frequency,
        double cutoff_frequency,
        double transition_width,
        int decimation_factor,
        int input_buffer_size_without_overlap,
        int output_offset,
        std::vector<float> *output_re_buffer,
        std::vector<float> *output_im_buffer,
        bool use_simd);

    FirFilterStage(const FirFilterStage &) = delete;
    FirFilterStage &operator=(const FirFilterStage &) = delete;
    FirFilterStage(FirFilterStage &&) = delete;
    FirFilterStage &operator=(FirFilterStage &&) = delete;

    ~FirFilterStage();

    std::string name() const;
    [[nodiscard]] int filterSize() const;
    int decimationFactor() const;
    [[nodiscard]] std::vector<float> *inputReBuffer();
    [[nodiscard]] std::vector<float> *inputImBuffer();
    std::string toString() const;

    void setInput(int index, std::complex<float> v);

    void applyFilter();
    void moveDataToFront();

private:
    void firFilter(const float *input, size_t input_length,
        const float *filter, size_t filter_size, float *output, int decimation_factor);

    static void firFilterNormal(const float *input, size_t input_length,
        const float *filter, size_t filter_size, float *output, int decimation_factor);

    constexpr static int c_AVX_floats_per_chunk = 8;
    static void firFilterAvx(const float *input, size_t input_length,
        const float *filter, size_t filter_size, float *output, int decimation_factor);

    static constexpr int c_NEON_floats_per_chunk = 4;
    static void firFilterNeon(const float *input, size_t input_length,
        const float *filter, size_t filter_size, float *output, int decimation_factor);

    std::string m_name;
    double m_sample_frequency;
    double m_cutoff_frequency;
    double m_transition_width;
    std::vector<float> m_filter;
    int m_decimation_factor;
    int m_input_buffer_size_without_overlap;
    int m_output_offset;
    bool m_use_simd;

    // The input buffer is owned by the object and deallocated in the destructor
    std::vector<float> *m_input_re_buffer;
    std::vector<float> *m_input_im_buffer;

    std::vector<float> *m_output_re_buffer;
    std::vector<float> *m_output_im_buffer;
};

#endif //AC3RF_DECODE_FIRFILTERSTAGE_H
