//
// Adaptive equaliser for MUSE input samples at 16.2 MHz.
//
// Trains a 33-tap symmetric FIR against the VITS mono-pulse on lines 0–1.  VITS
// from two successive frames is interleaved to give an effective 32.4 MHz
// observation of the channel impulse response, then LMS gradient descent is run
// on the tap vector with a unit-DC-gain renormalisation so the existing rescale
// path is undisturbed.
//
// First cut: CPU only.  The apply() step is a candidate for porting to a Vulkan
// compute shader once the algorithm has been validated.
//

#ifndef MUSECPP_MUSEADAPTIVEEQUALIZER_H
#define MUSECPP_MUSEADAPTIVEEQUALIZER_H

#include <array>
#include <vector>

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

    // Reads VITS from the (unfiltered) input frame and, if in eAdapt mode and a
    // previous frame with a *different* C sub-sampling phase is buffered, runs one
    // LMS step.  phase_c is the value of frame_subsampling_phase_C from the decoded
    // control signal (0 or 1); pass -1 if the control signal could not be decoded.
    // Always caches this frame's VITS / phase for use by the next call.
    void updateFromFrame(float const *frame_data, int frame_no, int phase_c);

    // Filters the input frame in-place at 16.2 MHz using the current taps.
    // No-op if mode is eOff.
    void apply(float *frame_data, int frame_sample_count);

private:
    Logger &m_log;
    Mode m_mode;
    float m_alpha;
    std::array<float, c_num_taps> m_taps{};

    static constexpr int c_vits_len = FrameBuffer::c_vits_sample_count;
    std::array<float, c_vits_len> m_prev_vits_line0{};
    std::array<float, c_vits_len> m_prev_vits_line1{};
    bool m_have_prev_vits = false;
    int m_last_frame_no = -1;
    int m_prev_phase_c = -1;
    int m_updates = 0;
    float m_last_err_energy = 0.0f;
    float m_last_obs_peak = 0.0f;

    std::vector<float> m_scratch; // scratch buffer for in-place filtering

    void runLmsStep(const std::array<float, c_vits_len> &curr_line0,
                    const std::array<float, c_vits_len> &curr_line1,
                    int curr_phase_c);
};

#endif //MUSECPP_MUSEADAPTIVEEQUALIZER_H
