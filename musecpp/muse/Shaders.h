//
// Created by staffanu on 5/6/23.
//

#ifndef MUSECPP_SHADERS_H
#define MUSECPP_SHADERS_H

#include <string>
#include <vector>
#include <csignal>
#include "musevk/VulkanBuffer.h"
#include "musevk/CommandPool.h"

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
    Shaders(Logger &log, std::string const &executable_dir, musevk::VulkanManager &manager, musevk::CommandPool &command_pool);

    Shaders(Shaders &other) = delete;
    void operator=(const Shaders &) = delete;

    std::shared_ptr<musevk::VulkanBuffer> createMuseBuffer(unsigned int height, unsigned int width, musevk::HostAccess host_access = musevk::eHostNone);

    enum class DropoutMode {
        eNormal, eDisabled, eHighlight
    };

    void applyEqAndDeemphasisAndGamma(musevk::CommandBuffer &sq,
                                      std::shared_ptr <musevk::VulkanBuffer> const &video_input,
                                      std::shared_ptr <musevk::VulkanBuffer> const &dropout_input,
                                      std::shared_ptr <musevk::VulkanBuffer> const &buffer,
                                      const std::pair<float, float> &eq,
                                      bool enable_non_linear, DropoutMode dropout_mode);

    void decodeIntraField(musevk::CommandBuffer &sq, FieldBufferView &field);

    bool decodeInterFrameAndDetectMotion(musevk::CommandBuffer &sq,
                                         const std::vector<std::reference_wrapper<FieldBufferView>> &fields,
                                         bool use_prev_motion_info);

    void combineStillAndMovingParts(musevk::CommandBuffer &sq, bool force_field_only, bool force_inter_frame_only,
                                    unsigned int field_parity, bool output_yuv);

    struct ResultImages {
        std::shared_ptr<musevk::VulkanImage> out_image;
        std::shared_ptr<musevk::VulkanBuffer> out_Y;
        std::shared_ptr<musevk::VulkanBuffer> out_U;
        std::shared_ptr<musevk::VulkanBuffer> out_V;
    };

    ResultImages getResultImages();

    void convertAudioSampleRate(musevk::CommandBuffer &sq, std::shared_ptr<musevk::VulkanBuffer> const &frame);

    [[nodiscard]] std::shared_ptr<musevk::VulkanBuffer> getAudioData() const;

private:
    // phase is 0 if even rows should have even columns computed, 1 if odd rows should have even columns computed
    void copyYForInterpolation(musevk::CommandBuffer &sq, int descriptor_set_index,
                               std::shared_ptr<musevk::VulkanBuffer> const &frame,
                               std::shared_ptr<musevk::VulkanBuffer> const &output,
                               unsigned int field_parity, unsigned int frame_phase_y, bool zero_non_copied_entries);

    void filterImageDiamond(musevk::CommandBuffer &sq, int descriptor_set_index,
                            int phase, std::shared_ptr<musevk::VulkanBuffer> const &buffer);

    void makeFieldFromConsecutiveFrames(musevk::CommandBuffer &sq,
                                        int copy_y_descriptor_set_first_index,
                                        int convert_sample_rate_descriptor_set_index,
                                        FieldBufferView &field_a, unsigned int field_a_frame_phase_y,
                                        FieldBufferView &field_b, unsigned int field_b_frame_phase_y,
                                        unsigned int fields_parity, unsigned int fields_phases);

    Logger &m_log;
    musevk::VulkanManager &m_vulkan_manager;
    musevk::CommandPool &m_command_pool;

    std::shared_ptr<musevk::ComputeShader> m_apply_eq_and_non_linear_algo;
    std::shared_ptr<musevk::ComputeShader> m_apply_dropout_compensation_algo;
    std::shared_ptr<musevk::ComputeShader> m_apply_deemphasis_and_gamma_algo;
    std::shared_ptr<musevk::ComputeShader> m_copy_y_for_interpolation_algo;
    std::shared_ptr<musevk::ComputeShader> m_diamond_algo;
    std::shared_ptr<musevk::ComputeShader> m_convert_sample_rate_algo;
    std::shared_ptr<musevk::ComputeShader> m_decode_c_algo;
    std::shared_ptr<musevk::ComputeShader> m_filter_c_algo;
    std::shared_ptr<musevk::ComputeShader> m_decode_c_single_field_algo;
    std::shared_ptr<musevk::ComputeShader> m_detect_motion_algo;
    std::shared_ptr<musevk::ComputeShader> m_combine_still_and_moving_algo;

    // temporary data used for the output of non-linear processing to the de-emphasis filter
    std::shared_ptr<musevk::VulkanBuffer> m_non_linear_processed_buffer; // MUSE_TOTAL_HEIGHT * MUSE_TOTAL_WIDTH

    // temporary data used by the single field decoder and inter-frame interpolation
    std::shared_ptr<musevk::VulkanBuffer> m_interpolated32_buffer; // MUSE_BUF_HEIGHT * MUSE_BUF_Y_WIDTH * 2

    // output from single field decoder -- when combining the two results
    std::shared_ptr<musevk::VulkanBuffer> m_field_Y_buffer; // MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH * 3
    std::shared_ptr<musevk::VulkanBuffer> m_field_r_buffer; // MUSE_BUF_HEIGHT / 2 * MUSE_BUF_Y_WIDTH / 2
    std::shared_ptr<musevk::VulkanBuffer> m_field_b_buffer; // MUSE_BUF_HEIGHT / 2 * MUSE_BUF_Y_WIDTH / 2

    // output from inter-frame interpolation
    std::shared_ptr<musevk::VulkanBuffer> m_inter_frame_Y_buffer; // MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3
    std::shared_ptr<musevk::VulkanBuffer> m_inter_frame_r_buffer; // MUSE_BUF_HEIGHT * 2 * MUSE_BUF_Y_WIDTH
    std::shared_ptr<musevk::VulkanBuffer> m_inter_frame_b_buffer; // MUSE_BUF_HEIGHT * 2 * MUSE_BUF_Y_WIDTH

    int m_current_movement_buffer_index;
    std::vector<std::shared_ptr<musevk::VulkanBuffer>> m_movement_buffers; // MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3
    std::shared_ptr<musevk::VulkanBuffer> m_movement_edge_buffer; // MUSE_BUF_HEIGHT * MUSE_BUF_Y_WIDTH
    std::shared_ptr<musevk::VulkanBuffer> m_movement_coring_buffer; // MUSE_BUF_HEIGHT * MUSE_BUF_Y_WIDTH
    std::shared_ptr<musevk::VulkanBuffer> m_movement_enlarged_buffer; // MUSE_BUF_HEIGHT * MUSE_BUF_Y_WIDTH

    // used for final result
    std::shared_ptr<musevk::VulkanImage> m_image_out;
    std::shared_ptr<musevk::VulkanBuffer> m_image_Y_out; // only used if writing to file using ffmpeg
    std::shared_ptr<musevk::VulkanBuffer> m_image_U_out; // ..
    std::shared_ptr<musevk::VulkanBuffer> m_image_V_out; // ..

    // sample rate converted audio data
    std::shared_ptr<musevk::VulkanBuffer> m_audio_data;

    // filter definitions
    std::shared_ptr<musevk::VulkanBuffer> m_diamond_filter_buffer;
    std::shared_ptr<musevk::VulkanBuffer> m_filter_2_to_3_buffer;
    std::shared_ptr<musevk::VulkanBuffer> m_filter_4_to_3_buffer;
};

#endif //MUSECPP_SHADERS_H
