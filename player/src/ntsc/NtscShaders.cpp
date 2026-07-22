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
  m_field_Y_buffer(createVulkanBuffer(NTSC_FIELD_HEIGHT, NTSC_Y_BUF_WIDTH)),
  m_field_U_buffer(createVulkanBuffer(NTSC_FIELD_HEIGHT, NTSC_Y_BUF_WIDTH)),
  m_field_V_buffer(createVulkanBuffer(NTSC_FIELD_HEIGHT, NTSC_Y_BUF_WIDTH)),
  m_inter_frame_Y_buffer(createVulkanBuffer(NTSC_FIELD_HEIGHT * 2, NTSC_Y_BUF_WIDTH)),
  m_inter_frame_U_buffer(createVulkanBuffer(NTSC_FIELD_HEIGHT * 2, NTSC_Y_BUF_WIDTH)),
  m_inter_frame_V_buffer(createVulkanBuffer(NTSC_FIELD_HEIGHT * 2, NTSC_Y_BUF_WIDTH)),
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
                                          vk::BufferUsageFlagBits::eStorageBuffer, eHostRead)),

  // Filters created in Octave:
  // fsc = 3.5795e+06
  // W=[(fsc-1.2e6)/(4*fsc/2) (fsc+1.2e6)/(4*fsc/2)]
  // notch = fir1(30, W, 'stop'); bandpass = fir1(30, W, 'pass')
  m_y_c_notch_filter_buffer(VulkanUtil::createDeviceBufferFloatsAsHalfFloats(m_vulkan_manager, command_pool,
          Size(31),
          {
            0.00016, 0.00361, -0.00012, -0.00032, -0.00021, -0.01672, 0.00077, 0.03424,
            -0.00059, -0.00129, -0.00073, -0.11566, 0.00178, 0.26475, -0.00097, 0.66263,
            -0.00097, 0.26475, 0.00178, -0.11566, -0.00073, -0.00129, -0.00059, 0.03424,
            0.00077, -0.01672, -0.00021, -0.00032, -0.00012, 0.00361, 0.00016
          })),
  m_y_c_bandpass_filter_buffer(VulkanUtil::createDeviceBufferFloatsAsHalfFloats(m_vulkan_manager, command_pool,
          Size(31),
          {
              -0.00016, -0.00362, 0.00012, 0.00032, 0.00021, 0.01675, -0.00078, -0.03429,
              0.00059, 0.00130, 0.00073, 0.11585, -0.00178, -0.26520, 0.00097, 0.33472,
              0.00097, -0.26520, -0.00178, 0.11585, 0.00073, 0.00130, 0.00059, -0.03429,
             -0.00078, 0.01675, 0.00021, 0.00032, 0.00012, -0.00362, -0.00016
          }))
{
  m_copy_to_frame_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
          "ntsc_copy_to_frame",
          {eBuffer, eBuffer, eBuffer}, sizeof(uint32_t) * 1,
          VulkanUtil::loadSpirv(executable_dir, "ntsc_copy_to_frame.comp"), Size(NTSC_TOTAL_WIDTH, NTSC_TOTAL_HEIGHT)));
  m_detect_color_burst_phase_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
        "ntsc_detect_color_burst_phase",
        {eBuffer, eBuffer}, sizeof(uint32_t) * 0,
        VulkanUtil::loadSpirv(executable_dir, "ntsc_detect_color_burst_phase.comp"), Size(NTSC_TOTAL_HEIGHT)));
  m_filter_color_for_frame_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
          "filter_color_for_frame",
          {eBuffer, eBuffer, eBuffer, eBuffer, eBuffer}, sizeof(uint32_t) * 2,
          VulkanUtil::loadSpirv(executable_dir, "ntsc_filter_color_for_frame.comp"), Size(NTSC_TOTAL_WIDTH, NTSC_TOTAL_HEIGHT)));
  m_decode_single_field_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
          "ntsc_decode_single_field",
          {eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer}, sizeof(uint32_t) * 3,
          VulkanUtil::loadSpirv(executable_dir, "ntsc_decode_single_field.comp"), Size(NTSC_Y_BUF_WIDTH, NTSC_FIELD_HEIGHT)));
  m_decode_two_fields_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
          "ntsc_decode_two_fields",
          {eBuffer, eBuffer, eBuffer, eBuffer, eBuffer}, sizeof(uint32_t) * 1,
          VulkanUtil::loadSpirv(executable_dir, "ntsc_decode_two_fields.comp"), Size(NTSC_Y_BUF_WIDTH, NTSC_FIELD_HEIGHT * 2)));
  m_combine_still_and_moving_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
          "ntsc_combine_still_and_moving",
          {eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eImage, eBuffer, eBuffer, eBuffer},
          sizeof(uint32_t) * 4,
          VulkanUtil::loadSpirv(executable_dir, "ntsc_combine_still_and_moving.comp"), Size(NTSC_Y_BUF_WIDTH, NTSC_FIELD_HEIGHT * 2)));
}

std::shared_ptr<musevk::VulkanBuffer> NtscShaders::createVulkanBuffer(unsigned int height, unsigned int width, HostAccess host_access) {
  return make_unique<VulkanBuffer>(m_vulkan_manager, Size(width, height), 2 /* sizeof(float16) */, vk::BufferUsageFlagBits::eStorageBuffer, host_access);
}

void NtscShaders::copyToFrame(musevk::CommandBuffer &sq, std::shared_ptr<musevk::VulkanBuffer> const &video_input,
  std::shared_ptr<musevk::VulkanBuffer> const &dropout_input, std::shared_ptr<musevk::VulkanBuffer> const &buffer,
  DropoutMode dropout_mode) {

  m_copy_to_frame_algo->updateBufferDescriptorsInSet(0, {video_input, dropout_input, buffer});
  sq.enqueueComputeShader<int32_t>(m_copy_to_frame_algo,{ dropout_mode == DropoutMode::eNormal ? 0 : dropout_mode == DropoutMode::eDisabled ? 1 : 2 });
}

void NtscShaders::filterColorForFrame(musevk::CommandBuffer &sq, NtscFrame *frame) {

  m_filter_color_for_frame_algo->updateBufferDescriptorsInSet(0, {m_y_c_notch_filter_buffer, m_y_c_bandpass_filter_buffer,
                                                                  frame->data(), frame->y_data(), frame->c_data()});
  sq.enqueueComputeShader<uint32_t>(m_filter_color_for_frame_algo, { m_y_c_notch_filter_buffer->size().x_size, m_y_c_bandpass_filter_buffer->size().x_size });

  m_detect_color_burst_phase_algo->updateBufferDescriptorsInSet(0, { frame->data(), frame->burst_phase_data() });
  sq.enqueueComputeShader<uint32_t>(m_detect_color_burst_phase_algo, {});
}

void NtscShaders::decodeSingleField(CommandBuffer &sq, NtscFieldView &field, float rot_re, float rot_im) {
  int field_parity = field.m_field_parity;

  m_decode_single_field_algo->updateBufferDescriptorsInSet(0, {field.m_data, field.m_y_data, field.m_c_data, field.m_burst_phase_data, m_field_Y_buffer, m_field_U_buffer, m_field_V_buffer});
  sq.enqueueComputeShader<uint32_t>(m_decode_single_field_algo,
      { (uint32_t)field_parity, std::bit_cast<uint32_t>(rot_re), std::bit_cast<uint32_t>(rot_im) });
}

// There are 4 fields in the vector.  Index 0 is the newest.
bool NtscShaders::decodeTwoFieldsAndDetectMotion(CommandBuffer &sq,
                                              const vector<reference_wrapper<NtscFieldView>> &fields,
                                              bool use_prev_motion_info) {
    assert(fields.size() >= 4);

    m_decode_two_fields_algo->updateBufferDescriptorsInSet(0, {fields[0].get().m_data, fields[1].get().m_data, m_inter_frame_Y_buffer, m_inter_frame_U_buffer, m_inter_frame_V_buffer});
    sq.enqueueComputeShader<int32_t>(m_decode_two_fields_algo,{ fields[0].get().m_field_parity });

    return true;
}

void NtscShaders::combineStillAndMovingParts(CommandBuffer &sq, bool force_field_only, bool force_inter_frame_only,
                                             unsigned int field_parity, bool output_yuv) {
  m_image_out->enqueueTransitionLayout(sq, vk::ImageLayout::eGeneral,
                                       vk::PipelineStageFlagBits::eTopOfPipe,
                                       vk::PipelineStageFlagBits::eComputeShader,
                                       vk::AccessFlags(), vk::AccessFlagBits::eShaderWrite);
  m_combine_still_and_moving_algo->updateBufferDescriptorsInSet(
          0,
          {m_field_Y_buffer, m_field_U_buffer,
           m_field_V_buffer, m_inter_frame_Y_buffer,
           m_inter_frame_U_buffer, m_inter_frame_V_buffer,
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
