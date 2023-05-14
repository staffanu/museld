//
// Created by staffanu on 5/6/23.
//

#ifndef MUSECPP_SHADERS_H
#define MUSECPP_SHADERS_H


#include <string>
#include <vector>
#include <csignal>
#include "kompute/Kompute.hpp"

template<typename T> class MuseBuffer;

class Shaders {
public:
    static void CreateInstance();
    static Shaders &GetInstance();
    static kp::Manager &GetManager();
    static std::vector<uint32_t>CompileSource(const std::string &filename);

    // phase is 0 if even rows should have even columns computed, 1 if odd rows should have even columns computed
    void ApplyTransmissionGamma(MuseBuffer<float> &buffer);
    void FilterImageDiamond(int phase, MuseBuffer<float> &buffer);
    void FilterImage(unsigned int filter_height, unsigned int filter_width,
                     std::shared_ptr<kp::Tensor> &filter,
                     MuseBuffer<float> &source, MuseBuffer<float> &dest,
                     float border_value, float multiplier);
    void ConvertHorizSampleRate2to3(MuseBuffer<float> &source, MuseBuffer<float> &dest,
                                    unsigned int output_start_row, unsigned int output_row_step_size);
    void FillEmptyLines(MuseBuffer<float> &buffer, int phase);
    void CombineYandRB(MuseBuffer<float> &Y_data, MuseBuffer<float> &C_r_data,
                       MuseBuffer<float> &C_b_data, MuseBuffer<uint32_t> &out_rgb);
    void DecodeC(MuseBuffer<float> &input_frame, MuseBuffer<float> &C_r_data,
                 MuseBuffer<float> &C_b_data, int frame_phase_c, int field_parity);
    static constexpr int s_color_filter_height = 5;
    static constexpr int s_color_filter_width = 9;
    std::shared_ptr<kp::Tensor> m_color_filter_tensor;
private:
    Shaders();
    Shaders(Shaders &other) = delete;
    void operator=(const Shaders &) = delete;

    static Shaders *s_singleton;

    std::vector<uint32_t> m_apply_transmission_gamma_y_spirv;
    std::vector<uint32_t> m_apply_transmission_gamma_c_spirv;
    std::vector<uint32_t> m_diamond_spirv;
    std::vector<uint32_t> m_filter_image_spirv;
    std::vector<uint32_t> m_convert_2_to_3_spirv;
    std::vector<uint32_t> m_fill_empty_lines_spirv;
    std::vector<uint32_t> m_combine_y_and_rb_spirv;
    std::vector<uint32_t> m_decode_c_spirv;
    kp::Manager m_mgr;

    const unsigned int m_diamond_filter_height = 7;
    const unsigned int m_diamond_filter_width = 9;
    std::shared_ptr<kp::Tensor> m_diamond_filter_tensor;

    std::shared_ptr<kp::Tensor> m_filter_2_to_3_tensor;
};

#endif //MUSECPP_SHADERS_H
