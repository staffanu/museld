// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_CXEXPANDER_H
#define AC3RF_DECODE_CXEXPANDER_H

// CX noise reduction expander for laserdisc analog audio.
//
// Feed-forward stereo expander: a shared sidechain high-pass filters both
// channels, the larger channel's peak level drives a fast-attack/slow-release
// level estimate, and the level maps to a gain between 0.2 and 3.16 that is
// applied to both channels.  The filter poles and the level-to-gain curve are
// ported from a hardware implementation calibrated against CX discs; samples
// are in FM deviation units where 1.0 corresponds to 100 kHz deviation.
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
    // Reference constants at the 48 kHz the hardware implementation ran at:
    // high-pass y = (15/16) y' + (31/32) (x - x'), attack alpha 1/32 per
    // sample toward 16x the peak, release factor 511/512 per sample.
    float m_hp_pole;
    static constexpr float c_hp_gain = 31.0f / 32.0f;
    static constexpr float c_hp_clamp = 1.22f;
    float m_attack_alpha;
    float m_release_factor;

    float m_hp_left, m_hp_right;
    float m_prev_left, m_prev_right;
    float m_level;
};

#endif //AC3RF_DECODE_CXEXPANDER_H
