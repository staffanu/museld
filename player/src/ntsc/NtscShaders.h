// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_NTSCSHADERS_H
#define MUSECPP_NTSCSHADERS_H

#include <string>

#include "musevk/VulkanManager.h"
#include "musevk/CommandPool.h"
#include "DropoutMode.h"
#include "NtscFieldView.h"
#include "NtscFrame.h"
#include "logging/Logger.h"
#include "ResultImages.h"

class NtscShaders {
public:
  NtscShaders(Logger &log, std::string const &executable_dir, musevk::VulkanManager &manager,
              musevk::CommandPool &command_pool);

  NtscShaders(NtscShaders &other) = delete;

  void operator=(const NtscShaders &) = delete;

  void copyToFrame(musevk::CommandBuffer &sq,
                   std::shared_ptr<musevk::VulkanBuffer> const &video_input,
                   std::shared_ptr<musevk::VulkanBuffer> const &dropout_input,
                   std::shared_ptr<musevk::VulkanBuffer> const &buffer,
                   DropoutMode dropout_mode);

  // Filter the raw frame data for luma and color using notch and bandpass filters respectively
  // Also fills in the color burst phase information
  void filterColorForFrame(musevk::CommandBuffer &sq, NtscFrame *frame);

  void decodeSingleField(musevk::CommandBuffer &sq, NtscFieldView &field, float rot_re, float rot_im);

  bool decodeTwoFieldsAndDetectMotion(musevk::CommandBuffer &sq,
                                     const std::vector<std::reference_wrapper<NtscFieldView>> &fields,
                                     bool use_prev_motion_info);

  void combineStillAndMovingParts(musevk::CommandBuffer &sq, bool force_field_only, bool force_inter_frame_only,
                                  unsigned int field_parity, bool output_yuv);

  ResultImages getResultImages();

private:
  std::shared_ptr<musevk::VulkanBuffer> createVulkanBuffer(unsigned int height, unsigned int width, musevk::HostAccess host_access = musevk::eHostNone);

  Logger &m_log;
  musevk::VulkanManager &m_vulkan_manager;

  std::shared_ptr<musevk::ComputeShader> m_copy_to_frame_algo;
  std::shared_ptr<musevk::ComputeShader> m_detect_color_burst_phase_algo;
  std::shared_ptr<musevk::ComputeShader> m_filter_color_for_frame_algo;
  std::shared_ptr<musevk::ComputeShader> m_decode_single_field_algo;
  std::shared_ptr<musevk::ComputeShader> m_decode_two_fields_algo;
  std::shared_ptr<musevk::ComputeShader> m_combine_still_and_moving_algo;

  // output from single field decoder
  std::shared_ptr<musevk::VulkanBuffer> m_field_Y_buffer; // NTSC_FIELD_HEIGHT * NTSC_Y_BUF_WIDTH
  std::shared_ptr<musevk::VulkanBuffer> m_field_U_buffer; // NTSC_FIELD_HEIGHT * NTSC_Y_BUF_WIDTH
  std::shared_ptr<musevk::VulkanBuffer> m_field_V_buffer; // NTSC_FIELD_HEIGHT * NTSC_Y_BUF_WIDTH

  // output from inter-frame interpolation
  std::shared_ptr<musevk::VulkanBuffer> m_inter_frame_Y_buffer; // NTSC_FIELD_HEIGHT * 2 * NTSC_Y_BUF_WIDTH
  std::shared_ptr<musevk::VulkanBuffer> m_inter_frame_U_buffer; // NTSC_FIELD_HEIGHT * 2 * NTSC_Y_BUF_WIDTH
  std::shared_ptr<musevk::VulkanBuffer> m_inter_frame_V_buffer; // NTSC_FIELD_HEIGHT * 2 * NTSC_Y_BUF_WIDTH

  int m_current_movement_buffer_index;
  std::vector<std::shared_ptr<musevk::VulkanBuffer>> m_movement_buffers; // MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3

  // used for final result
  std::shared_ptr<musevk::VulkanImage> m_image_out;
  std::shared_ptr<musevk::VulkanBuffer> m_image_Y_out; // only used if writing to file using ffmpeg
  std::shared_ptr<musevk::VulkanBuffer> m_image_U_out; // ..
  std::shared_ptr<musevk::VulkanBuffer> m_image_V_out; // ..

    // filter definitions
    std::shared_ptr<musevk::VulkanBuffer> m_y_c_notch_filter_buffer;
    std::shared_ptr<musevk::VulkanBuffer> m_y_c_bandpass_filter_buffer;
};


#endif //MUSECPP_NTSCSHADERS_H
