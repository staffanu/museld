// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <cmath>
#include "CxExpander.h"

CxExpander::CxExpander(double sample_frequency)
: m_hp_pole((float)std::exp(48000.0 * std::log(15.0 / 16.0) / sample_frequency)),
  m_attack_alpha((float)-std::expm1(48000.0 * std::log(31.0 / 32.0) / sample_frequency)),
  m_release_factor((float)std::exp(48000.0 * std::log(511.0 / 512.0) / sample_frequency)),
  m_hp_left(0), m_hp_right(0),
  m_prev_left(0), m_prev_right(0),
  m_level(0) {
}

float CxExpander::processSidechain(float left, float right) {
    m_hp_left = std::clamp(m_hp_pole * m_hp_left + c_hp_gain * (left - m_prev_left), -c_hp_clamp, c_hp_clamp);
    m_prev_left = left;
    m_hp_right = std::clamp(m_hp_pole * m_hp_right + c_hp_gain * (right - m_prev_right), -c_hp_clamp, c_hp_clamp);
    m_prev_right = right;

    const float current = std::max(std::abs(m_hp_left), std::abs(m_hp_right));
    if (16.0f * current > m_level)
        m_level += m_attack_alpha * (16.0f * current - m_level); // fast attack
    else
        m_level *= m_release_factor; // slow release

    // The hardware curve: gain = level * 5 / 4096 in its fixed-point units,
    // which is level * 0.511 with samples in FM deviation units.
    return std::clamp(m_level * 0.511f, 0.2f, 3.16f);
}
