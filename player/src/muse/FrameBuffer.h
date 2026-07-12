// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#ifndef MUSECPP_FRAMEBUFFER_H
#define MUSECPP_FRAMEBUFFER_H

#include <vector>
#include <string>
#include <cstdint>
#include <format>
#include "musevk/VulkanBuffer.h"
#include "DiscInfo.h"
#include "DiscCode.h"

class FieldBufferView;
class Logger;

class FrameBuffer {
public:
    FrameBuffer(Logger &log, int frame_no, std::shared_ptr<musevk::VulkanBuffer> data);

    static std::pair<float, float> EstimateRescale(float const *data);

    // VITS mono-pulses live on lines 1 and 2 (1-based) — that is rows 0 and 1 (0-based).
    // Pulse is at 1-based sample 264 (0-based index 263) in phase-C-0 frames, and one
    // 32.4 MHz cycle to the left of that in phase-C-1 frames (see MUSE－ハイビジョン伝送方式
    // §4.12 Fig. 4.87).
    //
    // We extract a 51-sample window centred on the pulse, covering 0-based indices
    // 238..288.  The window is narrower than the spec VITS region (215..315):
    // the next signal element after the VIT pulse begins ~50 samples
    // away, and the bandlimited ringing from its leading edge would otherwise
    // contaminate the trailing half of our extraction (empirically visible as a large
    // positive spike near 16.2 MHz index 310 in phase-C-0 frames).  Cutting half-way
    // between the two pulses bounds the contribution of either to roughly equal
    // levels at the window edges; we mirror the same cut on the left for symmetry
    // even though the lead-in is flat further back than that.
    //
    // The extra sample at the low end (i.e. one before the pulse-centred extent)
    // is needed by the phase-1 frame in the 32.4 MHz interleave: because phase 1 is
    // shifted half a sample left, phase-1 sample n contributes to the same 32.4 MHz
    // slot in phase-0's reference grid as phase-0 sample n+1 would — so phase-1's
    // useful range is one sample earlier than phase-0's.
    static constexpr int c_vits_first_sample = 238;
    static constexpr int c_vits_sample_count = 51;
    // Offset within the extracted window where the phase-0 pulse peak sits.
    static constexpr int c_vits_pulse_offset = 263 - c_vits_first_sample; // = 49
    static void ExtractVits(float const *data, float *out_line0, float *out_line1);

    void set_frame_no(int frame_no, long input_offset, double input_samples_per_sample);
    [[nodiscard]] long getInputOffset() const;
    [[nodiscard]] double getInputSamplesPerMuseSample() const;
    std::shared_ptr<musevk::VulkanBuffer> &data();
    FieldBufferView &get_field(int parity);
    [[nodiscard]] std::shared_ptr<DiscCode> getDiscCode() const;
    void ProcessControlData(float const *frame_data, std::pair<float, float> const &rescale);
    void processDiscCode();

private:
    int m_frame_no;
    long m_input_offset;
    double m_input_samples_per_sample;
    std::shared_ptr<musevk::VulkanBuffer> m_data;
    std::vector<FieldBufferView> m_fields;
    std::shared_ptr<DiscCode> m_disc_code;
};

#endif //MUSECPP_FRAMEBUFFER_H
