// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_NTSCFRAME_H
#define MUSECPP_NTSCFRAME_H


#include "logging/Logger.h"
#include "musevk/VulkanManager.h"
#include "NtscFieldView.h"
#include "VbiData.h"

class NtscFrame {
public:
    NtscFrame(Logger &log, int frame_no, musevk::VulkanManager &manager);

    // Robust noise sigmas measured on the flat reference regions of the raw
    // input frame, in the reader's voltage units (0.0 = sync tip, 0.3 =
    // blanking, 1.0 = white).  The NTSC RF demodulator has already applied
    // de-emphasis, so this is noise as it reaches the picture.
    struct NoiseEstimate {
        float sigma_blanking; // back porch windows of the picture lines
        float sigma_sync;     // sync tip windows
        float blanking_level; // robust blanking level, for tracking wander
    };
    static NoiseEstimate EstimateNoise(float const *data);

    // Accumulates the power spectrum of blanking-level windows on blank VBI
    // lines into psd[256] (bin k = k/256 × 14.318 MHz; for white noise of
    // variance σ² every bin converges to σ²).  Windows are only used when flat
    // and near the blanking level; max_sigma gates out VBI lines carrying
    // signal.  Returns the number of windows added.
    static int AccumulateNoisePsd(float const *data, double *psd, float max_sigma);

    void set_frame_no(int frame_no, long input_offset, double input_samples_per_sample);
    [[nodiscard]] long getInputOffset() const;
    [[nodiscard]] double getInputSamplesPerNtscSample() const;
    std::shared_ptr<musevk::VulkanBuffer> &data();
    std::shared_ptr<musevk::VulkanBuffer> &y_data();
    std::shared_ptr<musevk::VulkanBuffer> &burst_phase_data();
    std::shared_ptr<musevk::VulkanBuffer> &c_data();
    NtscFieldView &get_field(int parity);
    [[nodiscard]] std::shared_ptr<VbiData> getVbiData() const;
    void processVbi();

private:
    int processVbiLine(int line);

    int m_frame_no;
    long m_input_offset;
    double m_input_samples_per_sample;
    std::shared_ptr<musevk::VulkanBuffer> m_data;
    std::shared_ptr<musevk::VulkanBuffer> m_burst_phase_data;
    std::shared_ptr<musevk::VulkanBuffer> m_y_data; // Filtered by notch filter
    std::shared_ptr<musevk::VulkanBuffer> m_c_data; // Filtered by bandpass filter
    std::vector<NtscFieldView> m_fields;
    std::shared_ptr<VbiData> m_vbi_data;
};


#endif //MUSECPP_NTSCFRAME_H
