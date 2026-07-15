// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

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
    m_updates = 0;
    m_last_err_energy = 0;
    m_last_obs_peak = 0;
}

void MuseAdaptiveEqualizer::updateFromFrame(float const *frame_data, int /*frame_no*/, int phase_c) {
    if (m_mode != Mode::eAdapt || phase_c < 0)
        return;
    std::array<float, c_vits_len> curr_line0{};
    std::array<float, c_vits_len> curr_line1{};
    FrameBuffer::ExtractVits(frame_data, curr_line0.data(), curr_line1.data());
    runLmsStep(curr_line0, curr_line1, phase_c);
}

void MuseAdaptiveEqualizer::runLmsStep(
        const std::array<float, c_vits_len> &curr_line0,
        const std::array<float, c_vits_len> &curr_line1,
        int curr_phase_c) {
    // Build combined VITS by averaging the two lines (with sign flip — line 0 carries
    // the negative-going mono-pulse, line 1 the positive).  Zero-mean each line first
    // to decouple from rescale.
    std::array<float, c_vits_len> l0 = curr_line0;
    std::array<float, c_vits_len> l1 = curr_line1;
    zeroMean(l0.data(), c_vits_len);
    zeroMean(l1.data(), c_vits_len);
    std::array<float, c_vits_len> obs{};
    for (int i = 0; i < c_vits_len; i++)
        obs[i] = 0.5f * (l1[i] - l0[i]);

    // Per the bit-vs-spec swap discovered empirically: frame_subsampling_phase_C == 1
    // is the on-sample frame, phase_c == 0 is the half-sample-left frame.
    const bool on_sample = (curr_phase_c == 1);
    auto const &ref_pulse = on_sample ? MuseEqReference::onSamplePulse()
                                      : MuseEqReference::offSamplePulse();
    std::array<float, c_vits_len> ref{};
    std::copy(ref_pulse.begin(), ref_pulse.end(), ref.begin());
    zeroMean(ref.data(), c_vits_len);

    float obs_peak = peakAbs(obs.data(), c_vits_len);
    float ref_peak = peakAbs(ref.data(), c_vits_len);
    m_last_obs_peak = obs_peak;
    if (obs_peak < 1e-3f || ref_peak < 1e-6f) {
        m_log.debug(eDecoder, "MUSE EQ: VITS energy too low, skipping update");
        return;
    }
    float scale = obs_peak / ref_peak;
    for (int k = 0; k < c_vits_len; k++) ref[k] *= scale;

    if ((m_updates % 30) == 0 && m_log.isEnabled(eDebug, eDecoder)) {
        std::string obs_str, ref_str;
        for (int k = 0; k < c_vits_len; k++) obs_str += std::format(" {:+.2f}", obs[k]);
        for (int k = 0; k < c_vits_len; k++) ref_str += std::format(" {:+.2f}", ref[k]);
        m_log.debug(eDecoder, std::format("eq obs[phase_c={}, peak={}]:{}",
                                          curr_phase_c, MuseEqReference::c_center, obs_str));
        m_log.debug(eDecoder, std::format("eq ref[phase_c={}, peak={}]:{}",
                                          curr_phase_c, MuseEqReference::c_center, ref_str));
    }

    // Apply current taps to observation: y[k] = Σ_n h[n] * obs[k - (n - center)]
    std::array<float, c_vits_len> y{};
    for (int k = 0; k < c_vits_len; k++) {
        float acc = 0;
        for (int n = 0; n < c_num_taps; n++) {
            int idx = k - (n - c_center_tap);
            if (idx >= 0 && idx < c_vits_len) acc += m_taps[n] * obs[idx];
        }
        y[k] = acc;
    }

    // Error e[k] = y[k] − ref[k]; gradient: g[n] = Σ_k e[k] · obs[k - (n - center)]
    std::array<float, c_vits_len> err{};
    float err_energy = 0;
    for (int k = 0; k < c_vits_len; k++) {
        err[k] = y[k] - ref[k];
        err_energy += err[k] * err[k];
    }
    m_last_err_energy = err_energy;

    float obs_energy = 0;
    for (int k = 0; k < c_vits_len; k++) obs_energy += obs[k] * obs[k];
    if (obs_energy < 1e-6f) return;

    std::array<float, c_num_taps> grad{};
    for (int n = 0; n < c_num_taps; n++) {
        float g = 0;
        for (int k = 0; k < c_vits_len; k++) {
            int idx = k - (n - c_center_tap);
            if (idx >= 0 && idx < c_vits_len) g += err[k] * obs[idx];
        }
        grad[n] = g;
    }

    float step = m_alpha / obs_energy;
    for (int n = 0; n < c_num_taps; n++)
        m_taps[n] -= step * grad[n];

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

