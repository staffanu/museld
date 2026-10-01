// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cstdint>
#include <bit>
#include "NtscConstants.h"
#include "NtscShaders.h"
#include "DropoutMode.h"

#include "musevk/VulkanUtil.h"

using namespace std;
using namespace musevk;

NtscShaders::NtscShaders(Logger &log, const std::string &executable_dir, musevk::VulkanManager &manager,
                         musevk::CommandPool &command_pool, const VideoStandard &standard)
: m_log(log),
  m_vulkan_manager(manager),
  m_standard(standard),
  m_field_Y_buffers({createVulkanBuffer(standard.field_lines, standard.y_buf_width),
                     createVulkanBuffer(standard.field_lines, standard.y_buf_width)}),
  m_field_U_buffers({createVulkanBuffer(standard.field_lines, standard.y_buf_width),
                     createVulkanBuffer(standard.field_lines, standard.y_buf_width)}),
  m_field_V_buffers({createVulkanBuffer(standard.field_lines, standard.y_buf_width),
                     createVulkanBuffer(standard.field_lines, standard.y_buf_width)}),
  m_raw_past_buffer(createVulkanBuffer(standard.field_lines * 2, standard.y_buf_width)),
  m_raw_future_buffer(createVulkanBuffer(standard.field_lines * 2, standard.y_buf_width)),
  m_future_movement_buffer(createVulkanBuffer(standard.field_lines * 2, standard.y_buf_width)),
  m_current_movement_buffer_index(0),
  m_movement_buffers({ createVulkanBuffer(standard.field_lines * 2, standard.y_buf_width),
                     createVulkanBuffer(standard.field_lines * 2, standard.y_buf_width) }),

  m_image_out(make_unique<VulkanImage>(m_vulkan_manager,
                                       standard.y_buf_width, standard.field_lines * 2,
                                       vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc
                                       | vk::ImageUsageFlagBits::eTransferDst,
                                       eHostNone)),
  // eStorage only because VulkanImage always creates an image view, which
  // transfer-only usage would not permit
  m_image_held(make_unique<VulkanImage>(m_vulkan_manager,
                                        standard.y_buf_width, standard.field_lines * 2,
                                        vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc
                                        | vk::ImageUsageFlagBits::eTransferDst,
                                        eHostNone)),
  m_image_Y_out(make_unique<VulkanBuffer>(m_vulkan_manager, Size(standard.y_buf_width, standard.field_lines * 2), 2,
                                          vk::BufferUsageFlagBits::eStorageBuffer, eHostRead)),
  m_image_U_out(make_unique<VulkanBuffer>(m_vulkan_manager, Size(standard.y_buf_width / 2, standard.field_lines), 2,
                                          vk::BufferUsageFlagBits::eStorageBuffer, eHostRead)),
  m_image_V_out(make_unique<VulkanBuffer>(m_vulkan_manager, Size(standard.y_buf_width / 2, standard.field_lines), 2,
                                          vk::BufferUsageFlagBits::eStorageBuffer, eHostRead)),
  m_dropout_bits(make_unique<VulkanBuffer>(m_vulkan_manager, Size(standard.dropout_bit_words, standard.total_lines), sizeof(uint32_t),
                                           vk::BufferUsageFlagBits::eStorageBuffer, eHostNone)),
  m_concealed_composite(make_unique<VulkanBuffer>(m_vulkan_manager, Size(standard.samples_per_line, standard.total_lines), sizeof(float),
                                                  vk::BufferUsageFlagBits::eStorageBuffer, eHostNone)),
  m_chroma_taps(make_unique<VulkanBuffer>(m_vulkan_manager, Size(standard.y_buf_width + 2 * NTSC_CHROMA_TAP_HALO, standard.field_lines), 4 * 2 /* f16vec4 */,
                                          vk::BufferUsageFlagBits::eStorageBuffer, eHostNone))
{
  // The standard's build of each shader: ntsc_X.comp.spv or pal_X.comp.spv
  auto spirv = [&](const char *name) {
    return VulkanUtil::loadSpirv(executable_dir, std::string(standard.shader_prefix) + "_" + name + ".comp");
  };
  const int chroma_taps_width = standard.y_buf_width + 2 * NTSC_CHROMA_TAP_HALO;
  m_pack_dropout_bits_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager,
          "ntsc_pack_dropout_bits",
          {eBuffer, eBuffer}, sizeof(uint32_t) * 0,
          spirv("pack_dropout_bits"), Size(standard.dropout_bit_words, standard.total_lines)));
  m_extend_dropouts_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager,
          "ntsc_extend_dropouts",
          {eBuffer, eBuffer}, sizeof(uint32_t) * 0,
          spirv("extend_dropouts"), Size(standard.samples_per_line, standard.total_lines)));
  m_conceal_composite_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager,
          "ntsc_conceal_composite",
          {eBuffer, eBuffer, eBuffer, eBuffer, eBuffer}, sizeof(uint32_t) * 2,
          spirv("conceal_composite"), Size(standard.samples_per_line, standard.total_lines)));
  // one-dimensional over the flattened frame: the causal filter runs on in
  // scan order across row ends
  m_deemphasis_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager,
          "ntsc_deemphasis",
          {eBuffer, eBuffer}, sizeof(uint32_t) * 2,
          spirv("deemphasis"), Size((standard.samples_per_line * standard.total_lines + 3) / 4 /* PER_INVOCATION */)));
  m_chroma_taps_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager,
          "ntsc_chroma_taps",
          {eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer}, sizeof(uint32_t) * 1,
          spirv("chroma_taps"), Size(chroma_taps_width, standard.field_lines)));
  m_detect_color_burst_phase_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager,
        "ntsc_detect_color_burst_phase",
        {eBuffer, eBuffer}, sizeof(uint32_t) * 0,
        spirv("detect_color_burst_phase"), Size(standard.total_lines)));
  m_decode_single_field_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager,
          "ntsc_decode_single_field",
          {eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer}, sizeof(uint32_t) * 8,
          spirv("decode_single_field"), Size(standard.y_buf_width, standard.field_lines)));
  m_detect_motion_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager,
          "ntsc_detect_motion",
          {eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer}, sizeof(uint32_t) * 4,
          spirv("detect_motion"), Size(standard.y_buf_width, standard.field_lines * 2)));
  m_combine_still_and_moving_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager,
          "ntsc_combine_still_and_moving",
          {eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eImage, eBuffer, eBuffer, eBuffer},
          sizeof(uint32_t) * 4,
          spirv("combine_still_and_moving"), Size(standard.y_buf_width, standard.field_lines * 2)));
}

std::shared_ptr<musevk::VulkanBuffer> NtscShaders::createVulkanBuffer(unsigned int height, unsigned int width, HostAccess host_access) {
  return make_unique<VulkanBuffer>(m_vulkan_manager, Size(width, height), 2 /* sizeof(float16) */, vk::BufferUsageFlagBits::eStorageBuffer, host_access);
}

void NtscShaders::extendDropouts(musevk::CommandBuffer &sq, std::shared_ptr<musevk::VulkanBuffer> const &dropout_input,
  std::shared_ptr<musevk::VulkanBuffer> const &dropout_plane) {
  m_pack_dropout_bits_algo->updateBufferDescriptorsInSet(0, {dropout_input, m_dropout_bits});
  sq.enqueueComputeShader<uint32_t>(m_pack_dropout_bits_algo, {});
  m_extend_dropouts_algo->updateBufferDescriptorsInSet(0, {m_dropout_bits, dropout_plane});
  sq.enqueueComputeShader<uint32_t>(m_extend_dropouts_algo, {});
}

void NtscShaders::copyToFrame(musevk::CommandBuffer &sq, std::shared_ptr<musevk::VulkanBuffer> const &video_input,
  std::shared_ptr<musevk::VulkanBuffer> const &dropout_plane, std::shared_ptr<musevk::VulkanBuffer> const &buffer,
  DropoutMode dropout_mode, float level_offset_v, float level_scale) {

  m_conceal_composite_algo->updateBufferDescriptorsInSet(0, {video_input, dropout_plane, m_concealed_composite,
      m_movement_buffers[m_current_movement_buffer_index], m_future_movement_buffer});
  sq.enqueueComputeShader<uint32_t>(m_conceal_composite_algo,
      { dropout_mode == DropoutMode::eNormal ? 0u : dropout_mode == DropoutMode::eDisabled ? 1u : 2u,
        std::bit_cast<uint32_t>(level_offset_v) });
  m_deemphasis_algo->updateBufferDescriptorsInSet(0, {m_concealed_composite, buffer});
  sq.enqueueComputeShader<uint32_t>(m_deemphasis_algo,
      { std::bit_cast<uint32_t>(level_offset_v), std::bit_cast<uint32_t>(level_scale) });
}

void NtscShaders::detectColorBurstPhase(musevk::CommandBuffer &sq, NtscFrame *frame) {
  m_detect_color_burst_phase_algo->updateBufferDescriptorsInSet(0, { frame->data(), frame->burst_phase_data() });
  sq.enqueueComputeShader<uint32_t>(m_detect_color_burst_phase_algo, {});
}

void NtscShaders::decodeSingleField(CommandBuffer &sq, NtscFieldView &field,
                                    std::shared_ptr<musevk::VulkanBuffer> const &prev_frame,
                                    std::shared_ptr<musevk::VulkanBuffer> const &next_frame,
                                    std::shared_ptr<musevk::VulkanBuffer> const &prev_burst,
                                    std::shared_ptr<musevk::VulkanBuffer> const &next_burst,
                                    std::shared_ptr<musevk::VulkanBuffer> const &prev_dropout,
                                    std::shared_ptr<musevk::VulkanBuffer> const &next_dropout,
                                    DropoutMode dropout_mode, bool use_3d_comb,
                                    float rot_re, float rot_im, float level_floor, float level_ceiling,
                                    float chroma_sel_floor) {
  int field_parity = field.m_field_parity;

  m_chroma_taps_algo->updateBufferDescriptorsInSet(0, {field.m_data, field.m_burst_phase_data, prev_frame, next_frame,
      m_movement_buffers[m_current_movement_buffer_index], m_future_movement_buffer,
      prev_burst, next_burst, m_chroma_taps});
  sq.enqueueComputeShader<uint32_t>(m_chroma_taps_algo, { (uint32_t)field_parity });

  m_decode_single_field_algo->updateBufferDescriptorsInSet(0, {field.m_data, field.m_burst_phase_data,
      m_field_Y_buffers[field_parity], m_field_U_buffers[field_parity], m_field_V_buffers[field_parity],
      field.m_dropout_data, prev_frame, next_frame,
      m_movement_buffers[m_current_movement_buffer_index], m_future_movement_buffer,
      prev_burst, next_burst, prev_dropout, next_dropout, m_chroma_taps});
  sq.enqueueComputeShader<uint32_t>(m_decode_single_field_algo,
      { (uint32_t)field_parity,
        dropout_mode == DropoutMode::eNormal ? 0u : dropout_mode == DropoutMode::eDisabled ? 1u : 2u,
        use_3d_comb ? 1u : 0u,
        std::bit_cast<uint32_t>(rot_re), std::bit_cast<uint32_t>(rot_im),
        std::bit_cast<uint32_t>(level_floor), std::bit_cast<uint32_t>(level_ceiling),
        std::bit_cast<uint32_t>(chroma_sel_floor) });
}

void NtscShaders::detectMotion(CommandBuffer &sq,
                               std::shared_ptr<musevk::VulkanBuffer> const &frame_next,
                               std::shared_ptr<musevk::VulkanBuffer> const &frame0,
                               std::shared_ptr<musevk::VulkanBuffer> const &frame1,
                               std::shared_ptr<musevk::VulkanBuffer> const &frame2,
                               bool use_prev_movement, float motion_none, float motion_full) {
    int out = 1 - m_current_movement_buffer_index; // the other buffer holds the previous mask
    m_detect_motion_algo->updateBufferDescriptorsInSet(0,
        {frame_next, frame0, frame1, frame2, m_raw_past_buffer, m_raw_future_buffer,
         m_movement_buffers[m_current_movement_buffer_index],
         m_movement_buffers[out], m_future_movement_buffer});
    for (uint32_t phase : {1u, 2u})
        sq.enqueueComputeShader<uint32_t>(m_detect_motion_algo,
            { phase, use_prev_movement ? 1u : 0u,
              std::bit_cast<uint32_t>(motion_none), std::bit_cast<uint32_t>(motion_full) });
    m_current_movement_buffer_index = out;
}

void NtscShaders::combineStillAndMovingParts(CommandBuffer &sq, bool force_field_only, bool force_inter_frame_only,
                                             unsigned int field_parity, bool output_yuv, float black_setup) {
  m_image_out->enqueueTransitionLayout(sq, vk::ImageLayout::eGeneral,
                                       vk::PipelineStageFlagBits::eTopOfPipe,
                                       vk::PipelineStageFlagBits::eComputeShader,
                                       vk::AccessFlags(), vk::AccessFlagBits::eShaderWrite);
  m_combine_still_and_moving_algo->updateBufferDescriptorsInSet(
          0,
          {m_field_Y_buffers[0], m_field_U_buffers[0], m_field_V_buffers[0],
           m_field_Y_buffers[1], m_field_U_buffers[1], m_field_V_buffers[1],
           m_movement_buffers[m_current_movement_buffer_index], m_future_movement_buffer, m_image_out,
           m_image_Y_out, m_image_U_out, m_image_V_out});
  sq.enqueueComputeShader(m_combine_still_and_moving_algo,
                          vector{force_field_only ? 1u : 0u, force_inter_frame_only ? 1u : 0u, field_parity, output_yuv ? 1u : 0u,
                                 std::bit_cast<unsigned int>(black_setup)});

  if (output_yuv) {
    m_image_Y_out->synchronizeForHostRead(sq);
    m_image_U_out->synchronizeForHostRead(sq);
    m_image_V_out->synchronizeForHostRead(sq);
  }
}

namespace {
    vk::ImageBlit fullImageBlit(const VideoStandard &standard) {
        vk::ImageBlit region;
        region.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        region.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        region.srcOffsets[1] = vk::Offset3D{standard.y_buf_width, standard.field_lines * 2, 1};
        region.dstOffsets[1] = vk::Offset3D{standard.y_buf_width, standard.field_lines * 2, 1};
        return region;
    }
}

void NtscShaders::saveCombinedOutput(CommandBuffer &sq) {
  m_image_out->enqueueTransitionLayout(sq, vk::ImageLayout::eTransferSrcOptimal,
                                       vk::PipelineStageFlagBits::eComputeShader,
                                       vk::PipelineStageFlagBits::eTransfer,
                                       vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eTransferRead);
  m_image_held->enqueueTransitionLayout(sq, vk::ImageLayout::eTransferDstOptimal,
                                        vk::PipelineStageFlagBits::eTransfer,
                                        vk::PipelineStageFlagBits::eTransfer,
                                        vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eTransferWrite);
  sq.enqueueBlitImage(m_image_out->image(), vk::ImageLayout::eTransferSrcOptimal,
                      m_image_held->image(), vk::ImageLayout::eTransferDstOptimal, fullImageBlit(m_standard));
}

void NtscShaders::restoreHeldOutput(CommandBuffer &sq) {
  m_image_held->enqueueTransitionLayout(sq, vk::ImageLayout::eTransferSrcOptimal,
                                        vk::PipelineStageFlagBits::eTransfer,
                                        vk::PipelineStageFlagBits::eTransfer,
                                        vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferRead);
  m_image_out->enqueueTransitionLayout(sq, vk::ImageLayout::eTransferDstOptimal,
                                       vk::PipelineStageFlagBits::eTransfer,
                                       vk::PipelineStageFlagBits::eTransfer,
                                       vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eTransferWrite);
  sq.enqueueBlitImage(m_image_held->image(), vk::ImageLayout::eTransferSrcOptimal,
                      m_image_out->image(), vk::ImageLayout::eTransferDstOptimal, fullImageBlit(m_standard));
}

ResultImages NtscShaders::getResultImages() {
    return ResultImages { m_image_out, m_image_Y_out, m_image_U_out, m_image_V_out};
}
