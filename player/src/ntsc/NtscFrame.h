// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_NTSCFRAME_H
#define MUSECPP_NTSCFRAME_H


#include <array>
#include <optional>
#include <utility>

#include "logging/Logger.h"
#include "musevk/VulkanManager.h"
#include "NtscFieldView.h"
#include "VbiData.h"
#include "VideoStandard.h"

class NtscFrame {
public:
    NtscFrame(Logger &log, int frame_no, musevk::VulkanManager &manager, const VideoStandard &standard);

    // Robust noise sigmas measured on the flat reference regions of the raw
    // input frame, in the reader's voltage units (0.0 = sync tip, 0.3 =
    // blanking, 1.0 = white).  Measured before the frame-domain de-emphasis
    // (ntsc_deemphasis.comp), i.e. on the raw demodulated baseband.
    struct NoiseEstimate {
        float sigma_blanking; // back porch windows of the picture lines
        float sigma_sync;     // sync tip windows
        float blanking_level; // robust blanking level, for tracking wander
        float white_flag_level; // 100 IRE white flag level, -1 when no VBI line qualified
        // Histogram of the active picture's luma (one-subcarrier-cycle
        // means, so the chroma cancels) over the picture rows, relative to
        // the blanking level, in bins of c_luma_hist_bin volts from
        // c_luma_hist_min: the dark end, where the disc's black setup shows
        // as a peak at 7.5 IRE (NTSC-M) or at blanking (NTSC-J).  A peak is
        // what tells them apart: noise widens it but does not move it,
        // while it biases any percentile.
        static constexpr int c_luma_hist_bins = 48;
        static constexpr float c_luma_hist_bin = 0.0035f;   // 0.5 nominal IRE
        static constexpr float c_luma_hist_min = -0.028f;   // -4 IRE
        std::array<uint32_t, c_luma_hist_bins> luma_hist;
        // Colour burst phase against the sampling grid (line-alternation
        // unwrapped): the amplitude-weighted circular mean over the picture
        // lines and the spread around it.  The frame-to-frame mean tracks the
        // sampling phase coherence the 3D comb depends on.
        float burst_phase;       // radians
        float burst_phase_sigma; // radians
    };
    static NoiseEstimate EstimateNoise(float const *data, const VideoStandard &standard);

    // Accumulates the power spectrum of blanking-level windows on blank VBI
    // lines into psd[256] (bin k = k/256 × 14.318 MHz; for white noise of
    // variance σ² every bin converges to σ²).  Windows are only used when flat
    // and near the blanking level; max_sigma gates out VBI lines carrying
    // signal.  Returns the number of windows added.
    static int AccumulateNoisePsd(float const *data, double *psd, float max_sigma, const VideoStandard &standard);

    void set_frame_no(int frame_no, int64_t input_offset, double input_samples_per_sample);
    [[nodiscard]] int64_t getInputOffset() const;
    [[nodiscard]] double getInputSamplesPerNtscSample() const;
    std::shared_ptr<musevk::VulkanBuffer> &data();
    std::shared_ptr<musevk::VulkanBuffer> &burst_phase_data();
    std::shared_ptr<musevk::VulkanBuffer> &dropout_data();
    NtscFieldView &get_field(int parity);
    [[nodiscard]] std::shared_ptr<VbiData> getVbiData() const;
    // Field 1's EIA-608 closed caption byte pair (parity bits intact), sliced
    // from line 21 by processVbi(); nullopt when the line carries no caption
    // waveform.
    [[nodiscard]] std::optional<std::pair<uint8_t, uint8_t>> getClosedCaptionBytes() const;
    void processVbi();

private:
    int processVbiLine(int line);
    std::optional<std::pair<uint8_t, uint8_t>> processCcLine(int line);

    Logger &m_log;
    const VideoStandard &m_standard;
    int m_frame_no;
    int64_t m_input_offset;
    double m_input_samples_per_sample;
    std::shared_ptr<musevk::VulkanBuffer> m_data;
    std::shared_ptr<musevk::VulkanBuffer> m_burst_phase_data;
    std::shared_ptr<musevk::VulkanBuffer> m_dropout_data; // extended flags, written by the copy shader
    std::vector<NtscFieldView> m_fields;
    std::shared_ptr<VbiData> m_vbi_data;
    std::optional<std::pair<uint8_t, uint8_t>> m_cc_bytes;
};


#endif //MUSECPP_NTSCFRAME_H
