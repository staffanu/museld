//
// Created by staffanu on 5/6/23.
//

#ifndef MUSECPP_SHADERS_H
#define MUSECPP_SHADERS_H

#include <string>
#include <vector>
#include <csignal>
#include "MuseBuffer.h"
#include "VulkanResources.h"

class FieldBufferView;

class Shaders {
public:
    static std::vector<uint32_t> CompileSource(const std::string &filename);

    Shaders(musevk::VulkanResources &resources);

    Shaders(Shaders &other) = delete;

    void operator=(const Shaders &) = delete;

    musevk::VulkanResources &resources() {
        return m_vulkan_resources;
    };

    void ApplyTransmissionGamma(musevk::Sequence &sq, MuseBuffer<float> &buffer);

    void DecodeIntraField(musevk::Sequence &sq, FieldBufferView &field);

    bool DecodeInterFrameAndDetectMotion(musevk::Sequence &sq,
                                         std::vector<std::reference_wrapper<FieldBufferView>> const &fields);

    void CombineStillAndMovingParts(musevk::Sequence &sq, bool force_field_only, bool force_inter_frame_only);

    std::shared_ptr<musevk::Tensor> result_tensor();

private:
    template<typename T>
    MuseBuffer<T> CreateMuseBuffer(unsigned int height, unsigned int width);

    // phase is 0 if even rows should have even columns computed, 1 if odd rows should have even columns computed
    void CopyYForInterpolation(musevk::Sequence &sq,
                               MuseBuffer<float> &frame, MuseBuffer<float> &output,
                               unsigned int field_parity, unsigned int frame_phase_y, bool zero_non_copied_entries);

    void FilterImageDiamond(musevk::Sequence &sq,
                            int phase, MuseBuffer<float> &buffer);

    void FilterImage(musevk::Sequence &sq,
                     MuseBuffer<float> &filter,
                     MuseBuffer<float> &source, MuseBuffer<float> &dest,
                     float border_value, float multiplier);

    void ConvertHorizSampleRate4to1(musevk::Sequence &sq,
                                    MuseBuffer<float> &source, MuseBuffer<float> &dest);

    void DecodeC(musevk::Sequence &sq,
                 MuseBuffer<float> &input_frame,
                 MuseBuffer<float> &C_r_data, MuseBuffer<float> &C_b_data,
                 int frame_phase_c, int field_parity, bool zero_non_sample_points);

    void MakeFieldFromConsecutiveFrames(musevk::Sequence &sq,
                                        FieldBufferView &field_a, unsigned int field_a_frame_phase_y,
                                        FieldBufferView &field_b, unsigned int field_b_frame_phase_y,
                                        unsigned int fields_parity, unsigned int fields_phases);

    musevk::VulkanResources &m_vulkan_resources;

    std::vector<uint32_t> m_apply_transmission_gamma_y_spirv;
    std::vector<uint32_t> m_apply_transmission_gamma_c_spirv;
    std::vector<uint32_t> m_copy_y_for_interpolation_spirv;
    std::vector<uint32_t> m_diamond_spirv;
    std::vector<uint32_t> m_filter_image_spirv;
    std::vector<uint32_t> m_convert_horiz_sample_rate_spirv;
    std::vector<uint32_t> m_fill_empty_lines_spirv;
    std::vector<uint32_t> m_decode_c_spirv;
    std::vector<uint32_t> m_detect_motion_spirv;
    std::vector<uint32_t> m_combine_still_and_moving_spirv;

    std::shared_ptr<musevk::ComputeShader> m_fill_empty_lines_algo;
    std::shared_ptr<musevk::ComputeShader> m_convert_2_to_3_algo;
    std::shared_ptr<musevk::ComputeShader> m_convert_4_to_3_algo;
    std::shared_ptr<musevk::ComputeShader> m_combine_still_and_moving_algo;

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

    std::shared_ptr<musevk::Tensor> m_filter_2_to_3_tensor;
    std::shared_ptr<musevk::Tensor> m_filter_4_to_3_tensor;
    std::shared_ptr<musevk::Tensor> m_filter_4_to_1_tensor;
};

#endif //MUSECPP_SHADERS_H
