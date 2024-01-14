//
// Created by staffanu on 5/6/23.
//

#ifndef MUSECPP_SHADERS_H
#define MUSECPP_SHADERS_H

#include <string>
#include <vector>
#include <csignal>
#include "MuseBuffer.h"

class Logger;

namespace musevk {
    class VulkanImage;
    class CommandBuffer;
    class VulkanManager;
    class ComputeShader;
}
class FieldBufferView;

class Shaders {
public:
    static std::vector<uint32_t> loadSpirv(std::string const &executable_dir, std::string const &filename);

    Shaders(Logger &log, std::string const &executable_dir, musevk::VulkanManager &manager);

    Shaders(Shaders &other) = delete;
    void operator=(const Shaders &) = delete;

    MuseBuffer createMuseBuffer(unsigned int height, unsigned int width, bool host_visible = false);

    void convertToFloatAndApplyEqAndGamma(musevk::CommandBuffer &sq,
                                          std::shared_ptr<musevk::VulkanBuffer> input, MuseBuffer &buffer,
                                          std::pair<float, float> const &eq);

    void decodeIntraField(musevk::CommandBuffer &sq, FieldBufferView &field);

    bool decodeInterFrameAndDetectMotion(musevk::CommandBuffer &sq,
                                         const std::vector<std::reference_wrapper<FieldBufferView>> &fields,
                                         bool use_prev_motion_info);

    void combineStillAndMovingParts(musevk::CommandBuffer &sq, bool force_field_only, bool force_inter_frame_only);

    std::shared_ptr<musevk::VulkanImage> getResultImage();

    void convertAudioSampleRate(musevk::CommandBuffer &sq, MuseBuffer &frame);

    MuseBuffer &getAudioData();

private:
    // phase is 0 if even rows should have even columns computed, 1 if odd rows should have even columns computed
    void copyYForInterpolation(musevk::CommandBuffer &sq, int descriptor_set_index,
                               MuseBuffer &frame, MuseBuffer &output,
                               unsigned int field_parity, unsigned int frame_phase_y, bool zero_non_copied_entries);

    void filterImageDiamond(musevk::CommandBuffer &sq, int descriptor_set_index,
                            int phase, MuseBuffer &buffer);

    void filterImage(musevk::CommandBuffer &sq, int descriptor_set_index,
                     MuseBuffer &filter,
                     MuseBuffer &source, MuseBuffer &dest,
                     float border_value, float multiplier);

    void decodeC(musevk::CommandBuffer &sq, int descriptor_set_index,
                 MuseBuffer &input_frame,
                 int frame_phase_c, int field_parity, bool zero_non_sample_points);

    void makeFieldFromConsecutiveFrames(musevk::CommandBuffer &sq,
                                        int copy_y_descriptor_set_first_index,
                                        FieldBufferView &field_a, unsigned int field_a_frame_phase_y,
                                        FieldBufferView &field_b, unsigned int field_b_frame_phase_y,
                                        unsigned int fields_parity, unsigned int fields_phases);

    Logger &m_log;
    musevk::VulkanManager &m_vulkan_manager;

    std::vector<uint32_t> m_apply_eq_and_gamma_spriv;
    std::vector<uint32_t> m_copy_y_for_interpolation_spirv;
    std::vector<uint32_t> m_diamond_spirv;
    std::vector<uint32_t> m_filter_image_spirv;
    std::vector<uint32_t> m_convert_horiz_sample_rate_spirv;
    std::vector<uint32_t> m_fill_empty_lines_spirv;
    std::vector<uint32_t> m_decode_c_spirv;
    std::vector<uint32_t> m_detect_motion_spirv;
    std::vector<uint32_t> m_combine_still_and_moving_spirv;

    std::shared_ptr<musevk::ComputeShader> m_convert_to_float_and_apply_eq_and_gamma_algo;
    std::shared_ptr<musevk::ComputeShader> m_copy_y_for_interpolation_algo;
    std::shared_ptr<musevk::ComputeShader> m_diamond_algo;
    std::shared_ptr<musevk::ComputeShader> m_filter_image_algo;
    std::shared_ptr<musevk::ComputeShader> m_fill_empty_lines_algo;
    std::shared_ptr<musevk::ComputeShader> m_convert_sample_rate_algo;
    std::shared_ptr<musevk::ComputeShader> m_convert_sample_rate_4_to_3_algo;
    std::shared_ptr<musevk::ComputeShader> m_convert_sample_rate_2_to_3_algo;
    std::shared_ptr<musevk::ComputeShader> m_decode_c_algo;
    std::shared_ptr<musevk::ComputeShader> m_detect_motion_algo;
    std::shared_ptr<musevk::ComputeShader> m_combine_still_and_moving_algo;
    std::shared_ptr<musevk::ComputeShader> m_convert_audio_sample_rate_algo;

    // temporary data used by the single field decoder and inter-frame interpolation
    MuseBuffer m_interpolated32_buffer; // MUSE_BUF_HEIGHT * MUSE_BUF_Y_WIDTH * 2
    MuseBuffer m_intermediate_r_buffer; // MUSE_BUF_HEIGHT * MUSE_BUF_Y_WIDTH
    MuseBuffer m_intermediate_b_buffer; // MUSE_BUF_HEIGHT * MUSE_BUF_Y_WIDTH

    // output from single field decoder -- when combining the two results
    MuseBuffer m_field_Y_buffer;
    MuseBuffer m_field_r_buffer;
    MuseBuffer m_field_b_buffer;

    // output from inter-frame interpolation
    MuseBuffer m_inter_frame_Y_buffer; // MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3
    MuseBuffer m_inter_frame_r_buffer;
    MuseBuffer m_inter_frame_b_buffer;

    int m_current_movement_buffer_index;
    std::vector<MuseBuffer> m_movement_buffers; // MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3
    MuseBuffer m_movement_edge_buffer; // MUSE_BUF_HEIGHT * MUSE_BUF_Y_WIDTH
    MuseBuffer m_movement_coring_buffer; // MUSE_BUF_HEIGHT * MUSE_BUF_Y_WIDTH
    MuseBuffer m_movement_enlarged_buffer; // MUSE_BUF_HEIGHT * MUSE_BUF_Y_WIDTH

    // used for final result
    std::shared_ptr<musevk::VulkanImage> m_image_out;

    // sample rate converted audio data
    MuseBuffer m_audio_data;

    // filter definitions
    MuseBuffer m_diamond_filter_buffer;
    MuseBuffer m_color_filter_single_field_buffer;
    MuseBuffer m_color_filter_inter_frame_buffer;

    std::shared_ptr<musevk::VulkanBuffer> m_filter_2_to_3_buffer;
    std::shared_ptr<musevk::VulkanBuffer> m_filter_4_to_3_buffer;
};

#endif //MUSECPP_SHADERS_H
