// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_SDTVSHADERS_H
#define MUSECPP_SDTVSHADERS_H

#include <array>
#include <string>

#include "musevk/VulkanManager.h"
#include "musevk/CommandPool.h"
#include "DropoutMode.h"
#include "SdtvFieldView.h"
#include "SdtvFrame.h"
#include "logging/Logger.h"
#include "ResultImages.h"
#include "VideoStandard.h"

// The SD GPU pipeline.  The buffers are sized and the dispatches issued for
// the given standard's geometry, and the shaders loaded are that standard's
// build of the sdtv_*.comp sources (ntsc_*.spv or pal_*.spv).
class SdtvShaders {
public:
  SdtvShaders(Logger &log, std::string const &executable_dir, musevk::VulkanManager &manager,
              musevk::CommandPool &command_pool, const VideoStandard &standard);

  SdtvShaders(SdtvShaders &other) = delete;

  void operator=(const SdtvShaders &) = delete;

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
  void detectColorBurstPhase(musevk::CommandBuffer &sq, SdtvFrame *frame);

  void decodeSingleField(musevk::CommandBuffer &sq, SdtvFieldView &field,
                         std::shared_ptr<musevk::VulkanBuffer> const &prev_frame,
                         std::shared_ptr<musevk::VulkanBuffer> const &next_frame,
                         std::shared_ptr<musevk::VulkanBuffer> const &prev_burst,
                         std::shared_ptr<musevk::VulkanBuffer> const &next_burst,
                         std::shared_ptr<musevk::VulkanBuffer> const &prev_dropout,
                         std::shared_ptr<musevk::VulkanBuffer> const &next_dropout,
                         DropoutMode dropout_mode, bool use_3d_comb,
                         float rot_re, float rot_im, float level_floor, float level_ceiling,
                         float chroma_sel_floor, bool pal_v_flip);

  // Computes the per-pixel motion masks from the composite frame history into
  // the current movement buffer (flipping the ping-pong index).  The comb set
  // serves the temporal comb and the dropout paths, at the comb's frame
  // spacing; the weave set serves the de-interlacer, and exists only where
  // the two spacings differ (PAL): there it is computed from the consecutive
  // frames, with box_aligned set (see sdtv_detect_motion.comp)
  enum class MotionSet { eComb, eWeave };
  void detectMotion(musevk::CommandBuffer &sq, MotionSet set,
                    std::shared_ptr<musevk::VulkanBuffer> const &frame_next,
                    std::shared_ptr<musevk::VulkanBuffer> const &frame0,
                    std::shared_ptr<musevk::VulkanBuffer> const &frame1,
                    std::shared_ptr<musevk::VulkanBuffer> const &frame2,
                    bool use_prev_movement, float motion_none, float motion_full, bool box_aligned);

  void combineStillAndMovingParts(musevk::CommandBuffer &sq, bool force_field_only, bool force_inter_frame_only,
                                  unsigned int field_parity, bool output_yuv, float black_setup);

  // Keep a copy of the combine's output image, and bring it back.  A film
  // mode hold re-shows the previous film frame this way: the copy is taken
  // before the OSD and subtitles draw into the output image, and restoring
  // is idempotent, so a paused field can be redone any number of times.
  void saveCombinedOutput(musevk::CommandBuffer &sq);
  void restoreHeldOutput(musevk::CommandBuffer &sq);

  ResultImages getResultImages();

private:
  std::shared_ptr<musevk::VulkanBuffer> createVulkanBuffer(unsigned int height, unsigned int width, musevk::HostAccess host_access = musevk::eHostNone);

  Logger &m_log;
  musevk::VulkanManager &m_vulkan_manager;
  const VideoStandard &m_standard;

  std::shared_ptr<musevk::ComputeShader> m_pack_dropout_bits_algo;
  std::shared_ptr<musevk::ComputeShader> m_extend_dropouts_algo;
  std::shared_ptr<musevk::ComputeShader> m_conceal_composite_algo;
  std::shared_ptr<musevk::ComputeShader> m_deemphasis_algo;
  std::shared_ptr<musevk::ComputeShader> m_chroma_taps_algo;
  std::shared_ptr<musevk::ComputeShader> m_detect_color_burst_phase_algo;
  std::shared_ptr<musevk::ComputeShader> m_decode_single_field_algo;
  std::shared_ptr<musevk::ComputeShader> m_detect_motion_algo;
  std::shared_ptr<musevk::ComputeShader> m_combine_still_and_moving_algo;

  // one bit per column of raw dropout flags, the intermediate between the
  // pack and the extend shader (total_lines rows of dropout_bit_words)
  std::shared_ptr<musevk::VulkanBuffer> m_dropout_bits;

  // the dropout-concealed raw composite, between the conceal and the
  // de-emphasis shader (total_lines rows of samples_per_line floats)
  std::shared_ptr<musevk::VulkanBuffer> m_concealed_composite;

  // per-column chroma estimates for the field being decoded, between the
  // chroma taps and the single field shader (field_lines rows of
  // y_buf_width + 2 * c_chroma_tap_halo f16vec4)
  std::shared_ptr<musevk::VulkanBuffer> m_chroma_taps;

  // output from the single field decoder, one set per field parity so that
  // the previous field is still available for weaving in the combine
  std::array<std::shared_ptr<musevk::VulkanBuffer>, 2> m_field_Y_buffers; // field_lines * y_buf_width
  std::array<std::shared_ptr<musevk::VulkanBuffer>, 2> m_field_U_buffers;
  std::array<std::shared_ptr<musevk::VulkanBuffer>, 2> m_field_V_buffers;

  std::shared_ptr<musevk::VulkanBuffer> m_raw_past_buffer;   // field_lines * 2 * y_buf_width
  std::shared_ptr<musevk::VulkanBuffer> m_raw_future_buffer;
  std::shared_ptr<musevk::VulkanBuffer> m_future_movement_buffer;

  int m_current_movement_buffer_index;
  std::vector<std::shared_ptr<musevk::VulkanBuffer>> m_movement_buffers; // field_lines * 2, y_buf_width
  // the weave set (PAL only; on NTSC the combine reads the comb set)
  bool m_has_weave_masks;
  int m_current_weave_buffer_index;
  std::vector<std::shared_ptr<musevk::VulkanBuffer>> m_weave_movement_buffers;
  std::shared_ptr<musevk::VulkanBuffer> m_weave_future_movement_buffer;

  // used for final result
  std::shared_ptr<musevk::VulkanImage> m_image_out;
  std::shared_ptr<musevk::VulkanImage> m_image_held; // previous combine output, for film mode holds
  std::shared_ptr<musevk::VulkanBuffer> m_image_Y_out; // only used if writing to file using ffmpeg
  std::shared_ptr<musevk::VulkanBuffer> m_image_U_out; // ..
  std::shared_ptr<musevk::VulkanBuffer> m_image_V_out; // ..

    // filter definitions
};


#endif //MUSECPP_SDTVSHADERS_H
