//
// Created by staffanu on 5/6/23.
//

#ifndef MUSECPP_SHADERS_H
#define MUSECPP_SHADERS_H


#include <string>
#include <vector>
#include <csignal>
#include <opencv2/core/mat.hpp>
#include "kompute/Kompute.hpp"
#include "FieldBufferView.h"
#include "MuseBuffer.h"

class Shaders {
public:
    Shaders(Shaders &other) = delete;
    void operator=(const Shaders &) = delete;
    static void CreateInstance();
    static Shaders &GetInstance();
    static kp::Manager &GetManager();
    static std::vector<uint32_t>CompileSource(const std::string &filename);

    void ApplyTransmissionGamma(MuseBuffer<float> &buffer);
    cv::Mat DecodeIntraField(FieldBufferView &field);
    std::optional<cv::Mat> DecodeInterFrame(std::vector<std::reference_wrapper<FieldBufferView>> const &fields);
    std::optional<cv::Mat> DetectMotion(std::vector<std::reference_wrapper<FieldBufferView>> const &fields);
    std::optional<cv::Mat> CombineStillAndMovingParts();
private:
    Shaders();

    static Shaders *s_singleton;

    template<typename T> MuseBuffer<T> CreateMuseBuffer(unsigned int height, unsigned int width);

    // phase is 0 if even rows should have even columns computed, 1 if odd rows should have even columns computed
    void CopyYForInterpolation(std::shared_ptr<kp::Sequence> const &sq,
            MuseBuffer<float> &frame, MuseBuffer<float> &output,
            unsigned int field_parity, unsigned int frame_phase_y, bool zero_non_copied_entries);
    void FilterImageDiamond(std::shared_ptr<kp::Sequence> const &sq,
                            int phase, MuseBuffer<float> &buffer);
    void FilterImage(std::shared_ptr<kp::Sequence> const &sq,
                     MuseBuffer<float> &filter,
                     MuseBuffer<float> &source, MuseBuffer<float> &dest,
                     float border_value, float multiplier);
    void ConvertHorizSampleRate2to3(std::shared_ptr<kp::Sequence> const &sq,
                                    MuseBuffer<float> &source, MuseBuffer<float> &dest,
                                    unsigned int output_start_row);
    void ConvertHorizSampleRate4to3(std::shared_ptr<kp::Sequence> const &sq,
                                    MuseBuffer<float> &source, MuseBuffer<float> &dest,
                                    unsigned int output_start_row, unsigned int output_start_col);
    void ConvertHorizSampleRate4to1(std::shared_ptr<kp::Sequence> const &sq,
                                    MuseBuffer<float> &source, MuseBuffer<float> &dest);
    void FillEmptyLines(std::shared_ptr<kp::Sequence> const &sq,
                        MuseBuffer<float> &buffer, int phase);
    void CombineYandRB(std::shared_ptr<kp::Sequence> const &sq,
                       MuseBuffer<float> &Y_data, MuseBuffer<float> &C_r_data,
                       MuseBuffer<float> &C_b_data, MuseBuffer<uint32_t> &out_rgb);
    void DecodeC(std::shared_ptr<kp::Sequence> const &sq,
                 MuseBuffer<float> &input_frame,
                 MuseBuffer<float> &C_r_data, MuseBuffer<float> &C_b_data,
                 int frame_phase_c, int field_parity, bool zero_non_sample_points);

    void MakeFieldFromConsecutiveFrames(std::shared_ptr<kp::Sequence> const &sq,
                                        FieldBufferView &field_a, unsigned int field_a_frame_phase_y,
                                        FieldBufferView &field_b, unsigned int field_b_frame_phase_y,
                                        unsigned int fields_parity, unsigned int fields_phases);

    std::vector<uint32_t> m_apply_transmission_gamma_y_spirv;
    std::vector<uint32_t> m_apply_transmission_gamma_c_spirv;
    std::vector<uint32_t> m_copy_y_for_interpolation_spirv;
    std::vector<uint32_t> m_diamond_spirv;
    std::vector<uint32_t> m_filter_image_spirv;
    std::vector<uint32_t> m_convert_horiz_sample_rate_spirv;
    std::vector<uint32_t> m_fill_empty_lines_spirv;
    std::vector<uint32_t> m_combine_y_and_rb_spirv;
    std::vector<uint32_t> m_decode_c_spirv;
    std::vector<uint32_t> m_detect_motion_spirv;
    std::vector<uint32_t> m_combine_still_and_moving_spirv;

    kp::Manager m_mgr;

    // temporary data used by the single field decoder and inter-frame interpolation
    MuseBuffer<float> m_interpolated32_buffer; // MUSE_BUF_HEIGHT * MUSE_BUF_Y_WIDTH * 2
    MuseBuffer<float> m_intermediate_r_buffer;
    MuseBuffer<float> m_intermediate_b_buffer;

    // output from single field decoder -- when combining the two results
    MuseBuffer<float> m_field_Y_buffer;
    MuseBuffer<float> m_field_r_buffer;
    MuseBuffer<float> m_field_b_buffer;

    // output from inter-frame interpolation
    MuseBuffer<float> m_inter_frame_Y_buffer;
    MuseBuffer<float> m_inter_frame_r_buffer;
    MuseBuffer<float> m_inter_frame_b_buffer;

    MuseBuffer<float> m_movement_field_buffers[3]; // MUSE_BUF_HEIGHT * MUSE_BUF_Y_WIDTH / 2
    MuseBuffer<float> m_movement_buffer;

    // used for final result
    MuseBuffer<uint32_t> m_frame_out_buffer;

    // filter definitions
    MuseBuffer<float> m_diamond_filter_buffer;
    MuseBuffer<float> m_color_filter_single_field_buffer;
    MuseBuffer<float> m_color_filter_inter_frame_buffer;

    std::shared_ptr<kp::Tensor> m_filter_2_to_3_tensor;
    std::shared_ptr<kp::Tensor> m_filter_4_to_3_tensor;
    std::shared_ptr<kp::Tensor> m_filter_4_to_1_tensor;
};

#endif //MUSECPP_SHADERS_H
