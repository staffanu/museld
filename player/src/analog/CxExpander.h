// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_CXEXPANDER_H
#define AC3RF_DECODE_CXEXPANDER_H

// CX noise reduction expander for laserdisc analog audio.
//
// Feed-forward stereo expander: a shared sidechain high-pass filters both
// channels, the larger channel's peak level drives a dual-time-constant
// level estimate, and the level maps to the CX-14 gain curve -- 2:1 dB
// expansion from the -14 dB floor up to unity at the reference level, 1:1
// (unity) above it, per Fig. 28 of Pioneer's "Tuning Fork" No. 6, section
// 6.4 (reference nominally 40% modulation there).  The same section
// describes the decoder's detector as a 1 ms attack / 10 ms release filter
// followed by further LPF/HPF post-detection it does not specify; the
// sidechain here keeps the ~1 ms attack and stands in for the unspecified
// post-filtering with a fast release bounded by a slowly decaying floor,
// with the time constants and the reference point fit against the EFM
// track of a CX disc (see processSidechain).  Samples are in FM deviation
// units where 1.0 corresponds to 100 kHz deviation.
//
// The sidechain is cheap, so callers run processSidechain() on every sample
// and multiply the gain in only when CX is enabled; that keeps the level
// estimate warm across CX on/off transitions.
class CxExpander {
public:
    explicit CxExpander(double sample_frequency);

    // Advances the sidechain by one stereo sample and returns the CX gain.
    float processSidechain(float left, float right);

private:
    // Reference constants at 48 kHz (converted to the actual rate in the
    // constructor): high-pass y = (15/16) y' + (31/32) (x - x'), attack
    // alpha 1/64 per sample toward 16x the peak (~1.3 ms, matching the 1 ms
    // attack in Pioneer's CX description), fast release 2047/2048 per sample
    // bounded below by a slow floor releasing at 65535/65536.
    float m_hp_pole;
    static constexpr float c_hp_gain = 31.0f / 32.0f;
    static constexpr float c_hp_clamp = 1.22f;
    float m_attack_alpha;
    float m_release_fast;
    float m_release_slow;

    float m_hp_left, m_hp_right;
    float m_prev_left, m_prev_right;
    float m_level, m_slow_level;
};

#endif //AC3RF_DECODE_CXEXPANDER_H
