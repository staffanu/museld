// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <cmath>
#include "CxExpander.h"

CxExpander::CxExpander(double sample_frequency)
: m_hp_pole((float)std::exp(48000.0 * std::log(15.0 / 16.0) / sample_frequency)),
  m_attack_alpha((float)-std::expm1(48000.0 * std::log(63.0 / 64.0) / sample_frequency)),
  m_release_fast((float)std::exp(48000.0 * std::log(2047.0 / 2048.0) / sample_frequency)),
  m_release_slow((float)std::exp(48000.0 * std::log(65535.0 / 65536.0) / sample_frequency)),
  m_hp_left(0), m_hp_right(0),
  m_prev_left(0), m_prev_right(0),
  m_level(0), m_slow_level(0) {
}

float CxExpander::processSidechain(float left, float right) {
    m_hp_left = std::clamp(m_hp_pole * m_hp_left + c_hp_gain * (left - m_prev_left), -c_hp_clamp, c_hp_clamp);
    m_prev_left = left;
    m_hp_right = std::clamp(m_hp_pole * m_hp_right + c_hp_gain * (right - m_prev_right), -c_hp_clamp, c_hp_clamp);
    m_prev_right = right;

    const float current = 16.0f * std::max(std::abs(m_hp_left), std::abs(m_hp_right));

    // Dual-time-constant release: the level falls quickly, but never below a
    // slowly decaying floor that remembers the recent program level.  A
    // single release was audible as gain pumping ("amplitude vibrato"): fast
    // enough to track dynamics, it rode every syllable; slow enough not to,
    // it smeared them.  The split follows the level between notes without
    // chasing the level within them.
    if (current > m_level)
        m_level += m_attack_alpha * (current - m_level);
    else
        m_level = std::max(m_level * m_release_fast, m_slow_level);
    if (current > m_slow_level)
        m_slow_level += m_attack_alpha * (current - m_slow_level);
    else
        m_slow_level *= m_release_slow;

    // CX-14 static curve: linear-in-level gain is 2:1 expansion in dB,
    // active between the -14 dB floor (the LaserDisc variant's full noise
    // reduction) and unity at the reference level -- never above unity,
    // which is where the 1:1 region begins.  The scale calibrates the
    // reference point.  Both it and the sidechain time constants were fit
    // against the EFM track of a CX disc (Donna Summer X0-1): with them the
    // expanded analog track follows the EFM level within 0.6-0.7 dB RMS on
    // held-out material (the original single 1/512 release and 0.511 scale
    // with a 3.16 gain ceiling measured 1.8 dB, with audible pumping, and
    // played loud passages up to 10 dB hot).
    return std::clamp(m_level * 0.115f, 0.2f, 1.0f);
}
