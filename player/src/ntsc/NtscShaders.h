// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_NTSCSHADERS_H
#define MUSECPP_NTSCSHADERS_H

#include <array>
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

  void extendDropouts(musevk::CommandBuffer &sq,
                      std::shared_ptr<musevk::VulkanBuffer> const &dropout_input,
                      std::shared_ptr<musevk::VulkanBuffer> const &dropout_plane);

  void copyToFrame(musevk::CommandBuffer &sq,
                   std::shared_ptr<musevk::VulkanBuffer> const &video_input,
                   std::shared_ptr<musevk::VulkanBuffer> const &dropout_plane,
                   std::shared_ptr<musevk::VulkanBuffer> const &buffer,
                   DropoutMode dropout_mode, float level_offset_v, float level_scale);

  // Filter the raw frame data for luma and color using notch and bandpass filters respectively
  // Also fills in the color burst phase information
  void detectColorBurstPhase(musevk::CommandBuffer &sq, NtscFrame *frame);

  void decodeSingleField(musevk::CommandBuffer &sq, NtscFieldView &field,
                         std::shared_ptr<musevk::VulkanBuffer> const &prev_frame,
                         std::shared_ptr<musevk::VulkanBuffer> const &next_frame,
                         std::shared_ptr<musevk::VulkanBuffer> const &prev_burst,
                         std::shared_ptr<musevk::VulkanBuffer> const &next_burst,
                         std::shared_ptr<musevk::VulkanBuffer> const &prev_dropout,
                         std::shared_ptr<musevk::VulkanBuffer> const &next_dropout,
                         DropoutMode dropout_mode, bool use_3d_comb,
                         float rot_re, float rot_im, float level_floor, float level_ceiling,
                         float chroma_sel_floor);

  // Computes the per-pixel motion mask from the composite frame history into
  // the current movement buffer (flipping the ping-pong index)
  void detectMotion(musevk::CommandBuffer &sq,
                    std::shared_ptr<musevk::VulkanBuffer> const &frame_next,
                    std::shared_ptr<musevk::VulkanBuffer> const &frame0,
                    std::shared_ptr<musevk::VulkanBuffer> const &frame1,
                    std::shared_ptr<musevk::VulkanBuffer> const &frame2,
                    bool use_prev_movement, float motion_none, float motion_full);

  void combineStillAndMovingParts(musevk::CommandBuffer &sq, bool force_field_only, bool force_inter_frame_only,
                                  unsigned int field_parity, bool output_yuv);

  ResultImages getResultImages();

private:
  std::shared_ptr<musevk::VulkanBuffer> createVulkanBuffer(unsigned int height, unsigned int width, musevk::HostAccess host_access = musevk::eHostNone);

  Logger &m_log;
  musevk::VulkanManager &m_vulkan_manager;

  std::shared_ptr<musevk::ComputeShader> m_extend_dropouts_algo;
  std::shared_ptr<musevk::ComputeShader> m_copy_to_frame_algo;
  std::shared_ptr<musevk::ComputeShader> m_detect_color_burst_phase_algo;
  std::shared_ptr<musevk::ComputeShader> m_decode_single_field_algo;
  std::shared_ptr<musevk::ComputeShader> m_detect_motion_algo;
  std::shared_ptr<musevk::ComputeShader> m_combine_still_and_moving_algo;

  // output from the single field decoder, one set per field parity so that
  // the previous field is still available for weaving in the combine
  std::array<std::shared_ptr<musevk::VulkanBuffer>, 2> m_field_Y_buffers; // NTSC_FIELD_HEIGHT * NTSC_Y_BUF_WIDTH
  std::array<std::shared_ptr<musevk::VulkanBuffer>, 2> m_field_U_buffers;
  std::array<std::shared_ptr<musevk::VulkanBuffer>, 2> m_field_V_buffers;

  std::shared_ptr<musevk::VulkanBuffer> m_raw_past_buffer;   // NTSC_FIELD_HEIGHT * 2 * NTSC_Y_BUF_WIDTH
  std::shared_ptr<musevk::VulkanBuffer> m_raw_future_buffer;
  std::shared_ptr<musevk::VulkanBuffer> m_future_movement_buffer;

  int m_current_movement_buffer_index;
  std::vector<std::shared_ptr<musevk::VulkanBuffer>> m_movement_buffers; // NTSC_FIELD_HEIGHT * 2, NTSC_Y_BUF_WIDTH

  // used for final result
  std::shared_ptr<musevk::VulkanImage> m_image_out;
  std::shared_ptr<musevk::VulkanBuffer> m_image_Y_out; // only used if writing to file using ffmpeg
  std::shared_ptr<musevk::VulkanBuffer> m_image_U_out; // ..
  std::shared_ptr<musevk::VulkanBuffer> m_image_V_out; // ..

    // filter definitions
};


#endif //MUSECPP_NTSCSHADERS_H
