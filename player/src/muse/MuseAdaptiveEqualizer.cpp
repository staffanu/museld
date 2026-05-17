//
// Created for the MUSE adaptive equaliser; see MuseAdaptiveEqualizer.h.
//

#include "MuseAdaptiveEqualizer.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

#include "logging/Logger.h"
#include "MuseEqReference.h"

namespace {

// Subtract mean in place; returns the mean.
float zeroMean(float *x, int n) {
    float sum = 0;
    for (int i = 0; i < n; i++) sum += x[i];
    float mean = sum / (float)n;
    for (int i = 0; i < n; i++) x[i] -= mean;
    return mean;
}

// Peak absolute value.
float peakAbs(float const *x, int n) {
    float p = 0;
    for (int i = 0; i < n; i++) {
        float a = std::abs(x[i]);
        if (a > p) p = a;
    }
    return p;
}

} // namespace

MuseAdaptiveEqualizer::MuseAdaptiveEqualizer(Logger &log, Mode mode, float alpha)
        : m_log(log), m_mode(mode), m_alpha(alpha) {
    resetTaps();
}

MuseAdaptiveEqualizer::Mode MuseAdaptiveEqualizer::cycleMode() {
    switch (m_mode) {
        case Mode::eOff:    m_mode = Mode::eAdapt;  break;
        case Mode::eAdapt:  m_mode = Mode::eFrozen; break;
        case Mode::eFrozen: m_mode = Mode::eOff;    break;
    }
    return m_mode;
}

void MuseAdaptiveEqualizer::resetTaps() {
    m_taps.fill(0.0f);
    m_taps[c_center_tap] = 1.0f; // identity / pass-through
    m_have_prev_vits = false;
    m_last_frame_no = -1;
    m_prev_phase_c = -1;
    m_updates = 0;
    m_last_err_energy = 0;
    m_last_obs_peak = 0;
}

void MuseAdaptiveEqualizer::updateFromFrame(float const *frame_data, int frame_no, int phase_c) {
    std::array<float, c_vits_len> curr_line0{};
    std::array<float, c_vits_len> curr_line1{};
    FrameBuffer::ExtractVits(frame_data, curr_line0.data(), curr_line1.data());

    const bool consecutive = m_have_prev_vits && (frame_no == m_last_frame_no + 1);
    const bool phases_ok = phase_c >= 0 && m_prev_phase_c >= 0 && phase_c != m_prev_phase_c;
    if (m_mode == Mode::eAdapt && consecutive && phases_ok) {
        runLmsStep(curr_line0, curr_line1, phase_c);
    }

    m_prev_vits_line0 = curr_line0;
    m_prev_vits_line1 = curr_line1;
    m_have_prev_vits = true;
    m_last_frame_no = frame_no;
    m_prev_phase_c = phase_c;
}

void MuseAdaptiveEqualizer::runLmsStep(
        const std::array<float, c_vits_len> &curr_line0,
        const std::array<float, c_vits_len> &curr_line1,
        int curr_phase_c) {
    // The 32.4 MHz combined buffer holds 50 useful samples from each phase = 100 slots.
    // We extract c_vits_len = 51 samples per frame; phase-0 contributes its offsets
    // 1..c_vits_len-1, phase-1 contributes its offsets 0..c_vits_len-2.
    constexpr int n_useful = c_vits_len - 1;        // 50
    constexpr int n_obs    = 2 * n_useful;          // 100

    // Build per-frame combined VITS by averaging the two lines (with sign flip —
    // line 0 carries the negative-going mono-pulse, line 1 the positive).  We
    // zero-mean each line first to decouple from rescale.
    std::array<float, c_vits_len> l0 = curr_line0;
    std::array<float, c_vits_len> l1 = curr_line1;
    std::array<float, c_vits_len> p0 = m_prev_vits_line0;
    std::array<float, c_vits_len> p1 = m_prev_vits_line1;
    zeroMean(l0.data(), c_vits_len);
    zeroMean(l1.data(), c_vits_len);
    zeroMean(p0.data(), c_vits_len);
    zeroMean(p1.data(), c_vits_len);

    std::array<float, c_vits_len> curr_combined{};
    std::array<float, c_vits_len> prev_combined{};
    for (int i = 0; i < c_vits_len; i++) {
        curr_combined[i] = 0.5f * (l1[i] - l0[i]); // line 1 positive, line 0 negative
        prev_combined[i] = 0.5f * (p1[i] - p0[i]);
    }

    // Interleave by C sub-sampling phase.  Phase-1 frames are shifted left by one
    // 32.4 MHz cycle relative to phase-0 frames, so phase-1 samples occupy even
    // slots and phase-0 samples occupy the *following* odd slots in the 32.4 MHz
    // reconstruction.  Phase-0 contributes its extracted offsets 1..N-1 (so the
    // on-sample peak at offset c_vits_pulse_offset lands at obs slot
    // 2*(c_vits_pulse_offset - 1) + 1 = MuseEqReference::c_center), and phase-1
    // contributes its extracted offsets 0..N-2 one 32.4 MHz cycle earlier.
    // Empirically, frame_subsampling_phase_C == 1 corresponds to the spec's "on-sample"
    // VITS phase (peak exactly at sample 264), and phase_c == 0 is the half-sample-left
    // phase — opposite to a literal reading of the MuseBook §4.12 text.  Likely a
    // convention mismatch in our ControlSignalDecoder bit ordering.  Either way the bit
    // value is consistent across sessions, so we hard-code the swap here.
    const auto &phase0_vits = (curr_phase_c == 1) ? curr_combined : prev_combined;
    const auto &phase1_vits = (curr_phase_c == 1) ? prev_combined : curr_combined;

    // Phase-0 frame samples occupy odd 32.4 MHz slots starting from extracted offset 1
    // (so the on-sample peak at offset c_vits_pulse_offset lands at obs slot c_center).
    // Phase-1 frame samples occupy even slots starting from extracted offset 0 — they
    // are read one 16.2 MHz sample earlier in the extracted VITS window because phase 1
    // is shifted half a 16.2 MHz sample to the left.
    std::array<float, n_obs> obs{};
    for (int i = 0; i < n_useful; i++) {
        obs[2 * i + 0] = phase1_vits[i];       // phase-1 sample at extracted offset i
        obs[2 * i + 1] = phase0_vits[i + 1];   // phase-0 sample at extracted offset i+1
    }

    // Reference pulse: zero-mean, scale to same peak as observation so the LMS
    // step trains shape rather than overall amplitude.
    auto const &ref_pulse = MuseEqReference::pulse();
    std::array<float, n_obs> ref{};
    std::copy(ref_pulse.begin(), ref_pulse.end(), ref.begin());
    zeroMean(ref.data(), n_obs);

    float obs_peak = peakAbs(obs.data(), n_obs);
    float ref_peak = peakAbs(ref.data(), n_obs);
    m_last_obs_peak = obs_peak;
    if (obs_peak < 1e-3f || ref_peak < 1e-6f) {
        m_log.debug(eDecoder, "MUSE EQ: VITS energy too low, skipping update");
        return;
    }
    float scale = obs_peak / ref_peak;
    for (int k = 0; k < n_obs; k++) ref[k] *= scale;

    // One-shot debug dump on the first update and every 30th update thereafter, to
    // verify alignment of the observed pulse against the reference.  Look at where
    // the peaks line up — they should both sit at slot c_center.
    if ((m_updates % 30) == 0 && m_log.isEnabled(eDebug, eDecoder)) {
        std::string obs_str, ref_str;
        for (int k = 0; k < n_obs; k++) obs_str += std::format(" {:+.2f}", obs[k]);
        for (int k = 0; k < n_obs; k++) ref_str += std::format(" {:+.2f}", ref[k]);
        m_log.debug(eDecoder, std::format("eq obs[center={}]:{}", MuseEqReference::c_center, obs_str));
        m_log.debug(eDecoder, std::format("eq ref[center={}]:{}", MuseEqReference::c_center, ref_str));
    }

    // Apply current taps to observation: y[k] = Σ_n h[n] * obs[k - 2*(n - center)]
    // Taps live at 16.2 MHz spacing, so stride 2 over the 32.4 MHz observation.
    std::array<float, n_obs> y{};
    for (int k = 0; k < n_obs; k++) {
        float acc = 0;
        for (int n = 0; n < c_num_taps; n++) {
            int idx = k - 2 * (n - c_center_tap);
            if (idx >= 0 && idx < n_obs) acc += m_taps[n] * obs[idx];
        }
        y[k] = acc;
    }

    // Error e[k] = y[k] − ref[k]; gradient: g[n] = Σ_k e[k] · obs[k - 2*(n - center)]
    std::array<float, n_obs> err{};
    float err_energy = 0;
    for (int k = 0; k < n_obs; k++) {
        err[k] = y[k] - ref[k];
        err_energy += err[k] * err[k];
    }
    m_last_err_energy = err_energy;

    float obs_energy = 0;
    for (int k = 0; k < n_obs; k++) obs_energy += obs[k] * obs[k];
    if (obs_energy < 1e-6f) return;

    std::array<float, c_num_taps> grad{};
    for (int n = 0; n < c_num_taps; n++) {
        float g = 0;
        for (int k = 0; k < n_obs; k++) {
            int idx = k - 2 * (n - c_center_tap);
            if (idx >= 0 && idx < n_obs) g += err[k] * obs[idx];
        }
        grad[n] = g;
    }

    // Normalised-LMS-style step.
    float step = m_alpha / obs_energy;
    for (int n = 0; n < c_num_taps; n++) {
        m_taps[n] -= step * grad[n];
    }

    // Renormalise to unit DC gain so rescale parameters remain valid.
    float tap_sum = 0;
    for (float t : m_taps) tap_sum += t;
    if (std::abs(tap_sum) > 1e-6f) {
        for (float &t : m_taps) t /= tap_sum;
    }

    m_updates++;
}

float MuseAdaptiveEqualizer::deviationFromIdentity() const {
    float dev = 0;
    for (int n = 0; n < c_num_taps; n++) {
        float d = m_taps[n] - (n == c_center_tap ? 1.0f : 0.0f);
        dev += d * d;
    }
    return std::sqrt(dev);
}

void MuseAdaptiveEqualizer::apply(float *frame_data, int frame_sample_count) {
    if (m_mode == Mode::eOff) return;

    if ((int)m_scratch.size() < frame_sample_count)
        m_scratch.resize(frame_sample_count);

    // Direct-form FIR; ends are zero-padded.  Convention matches runLmsStep():
    //   y[i] = Σ_n h[n] · x[i − (n − center)] = Σ_n h[n] · x[i + center − n]
    // The frame buffer is a time-ordered 16.2 MHz sequence (line boundaries are
    // contiguous in time), so filtering across the whole buffer is correct.
    for (int i = 0; i < frame_sample_count; i++) {
        float acc = 0;
        for (int n = 0; n < c_num_taps; n++) {
            int idx = i + c_center_tap - n;
            if (idx >= 0 && idx < frame_sample_count) acc += m_taps[n] * frame_data[idx];
        }
        m_scratch[i] = acc;
    }
    std::copy_n(m_scratch.data(), frame_sample_count, frame_data);
}
