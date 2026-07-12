// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

//
// Adaptive equaliser for MUSE input samples at 16.2 MHz.
//
// Trains a 33-tap FIR against the VITS mono-pulse on lines 0–1.  On every frame
// the current frame's VITS is compared to the appropriate per-C-phase 16.2 MHz
// reference (raised cosine, β=0.1) and one normalised-LMS step is run.  Taps are
// renormalised to unit DC gain so the existing rescale path is undisturbed.
//
// The actual filter application runs on the GPU — see Shaders::applyEqualizer.
// This class only owns the tap vector and the adaptation logic.
//

#ifndef MUSECPP_MUSEADAPTIVEEQUALIZER_H
#define MUSECPP_MUSEADAPTIVEEQUALIZER_H

#include <array>

#include "FrameBuffer.h"

class Logger;

class MuseAdaptiveEqualizer {
public:
    enum class Mode {
        eOff,     // bypass — apply() is a no-op
        eAdapt,   // update taps on every frame, then apply
        eFrozen,  // apply with current taps, no updates
    };

    static constexpr int c_num_taps = 33;
    static constexpr int c_center_tap = c_num_taps / 2;

    MuseAdaptiveEqualizer(Logger &log, Mode mode, float alpha);

    void setMode(Mode mode) { m_mode = mode; }
    [[nodiscard]] Mode mode() const { return m_mode; }
    // Cycle off → adapt → frozen → off.  Returns the new mode.
    Mode cycleMode();
    // Reset taps to the identity (passthrough) filter and clear adaptation state.
    void resetTaps();
    [[nodiscard]] const std::array<float, c_num_taps> &taps() const { return m_taps; }
    [[nodiscard]] int updates() const { return m_updates; }
    [[nodiscard]] float lastErrorEnergy() const { return m_last_err_energy; }
    [[nodiscard]] float lastObsPeak() const { return m_last_obs_peak; }
    // L2 distance of current taps from the identity (delta) filter.
    [[nodiscard]] float deviationFromIdentity() const;

    // Reads VITS from the (unfiltered) input frame and, if in eAdapt mode, runs
    // one LMS step against the per-C-phase reference.  phase_c is the value of
    // frame_subsampling_phase_C from the decoded control signal (0 or 1); pass -1
    // if the control signal could not be decoded, in which case the step is
    // skipped.  frame_no is unused but kept for log/diagnostic continuity.
    //
    // The taps themselves are exposed via taps(); the actual filtering happens on
    // the GPU in Shaders::applyEqualizer.
    void updateFromFrame(float const *frame_data, int frame_no, int phase_c);

private:
    Logger &m_log;
    Mode m_mode;
    float m_alpha;
    std::array<float, c_num_taps> m_taps{};

    static constexpr int c_vits_len = FrameBuffer::c_vits_sample_count;
    int m_updates = 0;
    float m_last_err_energy = 0.0f;
    float m_last_obs_peak = 0.0f;

    void runLmsStep(const std::array<float, c_vits_len> &curr_line0,
                    const std::array<float, c_vits_len> &curr_line1,
                    int curr_phase_c);
};

#endif //MUSECPP_MUSEADAPTIVEEQUALIZER_H
