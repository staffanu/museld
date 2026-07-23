// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <bit>
#include "NtscConstants.h"
#include "NtscShaders.h"
#include "DropoutMode.h"

#include "musevk/VulkanUtil.h"

using namespace std;
using namespace musevk;

NtscShaders::NtscShaders(Logger &log, const std::string &executable_dir, musevk::VulkanManager &manager,
                         musevk::CommandPool &command_pool)
: m_log(log),
  m_vulkan_manager(manager),
  m_field_Y_buffers({createVulkanBuffer(NTSC_FIELD_HEIGHT, NTSC_Y_BUF_WIDTH),
                     createVulkanBuffer(NTSC_FIELD_HEIGHT, NTSC_Y_BUF_WIDTH)}),
  m_field_U_buffers({createVulkanBuffer(NTSC_FIELD_HEIGHT, NTSC_Y_BUF_WIDTH),
                     createVulkanBuffer(NTSC_FIELD_HEIGHT, NTSC_Y_BUF_WIDTH)}),
  m_field_V_buffers({createVulkanBuffer(NTSC_FIELD_HEIGHT, NTSC_Y_BUF_WIDTH),
                     createVulkanBuffer(NTSC_FIELD_HEIGHT, NTSC_Y_BUF_WIDTH)}),
  m_raw_motion_buffer(createVulkanBuffer(NTSC_FIELD_HEIGHT * 2, NTSC_Y_BUF_WIDTH)),
  m_current_movement_buffer_index(0),
  m_movement_buffers({ createVulkanBuffer(NTSC_FIELD_HEIGHT * 2, NTSC_Y_BUF_WIDTH),
                     createVulkanBuffer(NTSC_FIELD_HEIGHT * 2, NTSC_Y_BUF_WIDTH) }),

  m_image_out(make_unique<VulkanImage>(m_vulkan_manager,
                                       NTSC_Y_BUF_WIDTH, NTSC_FIELD_HEIGHT * 2,
                                       vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc,
                                       eHostNone)),
  m_image_Y_out(make_unique<VulkanBuffer>(m_vulkan_manager, Size(NTSC_Y_BUF_WIDTH, NTSC_FIELD_HEIGHT * 2), 2,
                                          vk::BufferUsageFlagBits::eStorageBuffer, eHostRead)),
  m_image_U_out(make_unique<VulkanBuffer>(m_vulkan_manager, Size(NTSC_Y_BUF_WIDTH / 2, NTSC_FIELD_HEIGHT), 2,
                                          vk::BufferUsageFlagBits::eStorageBuffer, eHostRead)),
  m_image_V_out(make_unique<VulkanBuffer>(m_vulkan_manager, Size(NTSC_Y_BUF_WIDTH / 2, NTSC_FIELD_HEIGHT), 2,
                                          vk::BufferUsageFlagBits::eStorageBuffer, eHostRead))
{
  m_extend_dropouts_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
          "ntsc_extend_dropouts",
          {eBuffer, eBuffer}, sizeof(uint32_t) * 0,
          VulkanUtil::loadSpirv(executable_dir, "ntsc_extend_dropouts.comp"), Size(NTSC_TOTAL_WIDTH, NTSC_TOTAL_HEIGHT)));
  m_copy_to_frame_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
          "ntsc_copy_to_frame",
          {eBuffer, eBuffer, eBuffer}, sizeof(uint32_t) * 3,
          VulkanUtil::loadSpirv(executable_dir, "ntsc_copy_to_frame.comp"), Size(NTSC_TOTAL_WIDTH, NTSC_TOTAL_HEIGHT)));
  m_detect_color_burst_phase_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
        "ntsc_detect_color_burst_phase",
        {eBuffer, eBuffer}, sizeof(uint32_t) * 0,
        VulkanUtil::loadSpirv(executable_dir, "ntsc_detect_color_burst_phase.comp"), Size(NTSC_TOTAL_HEIGHT)));
  m_decode_single_field_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
          "ntsc_decode_single_field",
          {eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer}, sizeof(uint32_t) * 6,
          VulkanUtil::loadSpirv(executable_dir, "ntsc_decode_single_field.comp"), Size(NTSC_Y_BUF_WIDTH, NTSC_FIELD_HEIGHT)));
  m_detect_motion_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
          "ntsc_detect_motion",
          {eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer}, sizeof(uint32_t) * 4,
          VulkanUtil::loadSpirv(executable_dir, "ntsc_detect_motion.comp"), Size(NTSC_Y_BUF_WIDTH, NTSC_FIELD_HEIGHT * 2)));
  m_combine_still_and_moving_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
          "ntsc_combine_still_and_moving",
          {eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eImage, eBuffer, eBuffer, eBuffer},
          sizeof(uint32_t) * 4,
          VulkanUtil::loadSpirv(executable_dir, "ntsc_combine_still_and_moving.comp"), Size(NTSC_Y_BUF_WIDTH, NTSC_FIELD_HEIGHT * 2)));
}

std::shared_ptr<musevk::VulkanBuffer> NtscShaders::createVulkanBuffer(unsigned int height, unsigned int width, HostAccess host_access) {
  return make_unique<VulkanBuffer>(m_vulkan_manager, Size(width, height), 2 /* sizeof(float16) */, vk::BufferUsageFlagBits::eStorageBuffer, host_access);
}

void NtscShaders::extendDropouts(musevk::CommandBuffer &sq, std::shared_ptr<musevk::VulkanBuffer> const &dropout_input,
  std::shared_ptr<musevk::VulkanBuffer> const &dropout_plane) {
  m_extend_dropouts_algo->updateBufferDescriptorsInSet(0, {dropout_input, dropout_plane});
  sq.enqueueComputeShader<uint32_t>(m_extend_dropouts_algo, {});
}

void NtscShaders::copyToFrame(musevk::CommandBuffer &sq, std::shared_ptr<musevk::VulkanBuffer> const &video_input,
  std::shared_ptr<musevk::VulkanBuffer> const &dropout_plane, std::shared_ptr<musevk::VulkanBuffer> const &buffer,
  DropoutMode dropout_mode, float level_offset_v, float level_scale) {

  m_copy_to_frame_algo->updateBufferDescriptorsInSet(0, {video_input, dropout_plane, buffer});
  sq.enqueueComputeShader<uint32_t>(m_copy_to_frame_algo,
      { dropout_mode == DropoutMode::eNormal ? 0u : dropout_mode == DropoutMode::eDisabled ? 1u : 2u,
        std::bit_cast<uint32_t>(level_offset_v), std::bit_cast<uint32_t>(level_scale) });
}

void NtscShaders::detectColorBurstPhase(musevk::CommandBuffer &sq, NtscFrame *frame) {
  m_detect_color_burst_phase_algo->updateBufferDescriptorsInSet(0, { frame->data(), frame->burst_phase_data() });
  sq.enqueueComputeShader<uint32_t>(m_detect_color_burst_phase_algo, {});
}

void NtscShaders::decodeSingleField(CommandBuffer &sq, NtscFieldView &field, DropoutMode dropout_mode,
                                    float rot_re, float rot_im, float level_floor, float level_ceiling) {
  int field_parity = field.m_field_parity;

  m_decode_single_field_algo->updateBufferDescriptorsInSet(0, {field.m_data, field.m_burst_phase_data,
      m_field_Y_buffers[field_parity], m_field_U_buffers[field_parity], m_field_V_buffers[field_parity],
      field.m_dropout_data});
  sq.enqueueComputeShader<uint32_t>(m_decode_single_field_algo,
      { (uint32_t)field_parity,
        dropout_mode == DropoutMode::eNormal ? 0u : dropout_mode == DropoutMode::eDisabled ? 1u : 2u,
        std::bit_cast<uint32_t>(rot_re), std::bit_cast<uint32_t>(rot_im),
        std::bit_cast<uint32_t>(level_floor), std::bit_cast<uint32_t>(level_ceiling) });
}

void NtscShaders::detectMotion(CommandBuffer &sq,
                               std::shared_ptr<musevk::VulkanBuffer> const &frame0,
                               std::shared_ptr<musevk::VulkanBuffer> const &frame1,
                               std::shared_ptr<musevk::VulkanBuffer> const &frame2,
                               bool use_prev_movement, float motion_none, float motion_full) {
    int out = 1 - m_current_movement_buffer_index; // the other buffer holds the previous mask
    m_detect_motion_algo->updateBufferDescriptorsInSet(0,
        {frame0, frame1, frame2, m_raw_motion_buffer, m_movement_buffers[m_current_movement_buffer_index],
         m_movement_buffers[out]});
    for (uint32_t phase : {1u, 2u})
        sq.enqueueComputeShader<uint32_t>(m_detect_motion_algo,
            { phase, use_prev_movement ? 1u : 0u,
              std::bit_cast<uint32_t>(motion_none), std::bit_cast<uint32_t>(motion_full) });
    m_current_movement_buffer_index = out;
}

void NtscShaders::combineStillAndMovingParts(CommandBuffer &sq, bool force_field_only, bool force_inter_frame_only,
                                             unsigned int field_parity, bool output_yuv) {
  m_image_out->enqueueTransitionLayout(sq, vk::ImageLayout::eGeneral,
                                       vk::PipelineStageFlagBits::eTopOfPipe,
                                       vk::PipelineStageFlagBits::eComputeShader,
                                       vk::AccessFlags(), vk::AccessFlagBits::eShaderWrite);
  m_combine_still_and_moving_algo->updateBufferDescriptorsInSet(
          0,
          {m_field_Y_buffers[0], m_field_U_buffers[0], m_field_V_buffers[0],
           m_field_Y_buffers[1], m_field_U_buffers[1], m_field_V_buffers[1],
           m_movement_buffers[m_current_movement_buffer_index], m_image_out,
           m_image_Y_out, m_image_U_out, m_image_V_out});
  sq.enqueueComputeShader(m_combine_still_and_moving_algo,
                          vector{force_field_only ? 1u : 0u, force_inter_frame_only ? 1u : 0u, field_parity, output_yuv ? 1u : 0u});

  if (output_yuv) {
    m_image_Y_out->synchronizeForHostRead(sq);
    m_image_U_out->synchronizeForHostRead(sq);
    m_image_V_out->synchronizeForHostRead(sq);
  }
}

ResultImages NtscShaders::getResultImages() {
    return ResultImages { m_image_out, m_image_Y_out, m_image_U_out, m_image_V_out};
}
