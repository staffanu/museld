// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "VbiData.h"
#include <format>

VbiData::VbiData(bool is_lead_in, bool is_lead_out, bool is_clv, bool is_stop_code,
    std::optional<int> chapter,
    std::optional<int> clv_time_seconds,
    std::optional<int> clv_picture_number,
    std::optional<int> cav_picture_number,
    std::optional<bool> cx_enabled)
: DiscInfo(),
  m_is_lead_in(is_lead_in),
  m_is_lead_out(is_lead_out),
  m_is_clv(is_clv),
  m_is_stop_code(is_stop_code),
  m_chapter(chapter),
  m_clv_time_seconds(clv_time_seconds),
  m_clv_picture_number(clv_picture_number),
  m_cav_picture_number(cav_picture_number),
  m_cx_enabled(cx_enabled) {
}

std::vector<std::string> VbiData::asStrings() const {
  std::string string1 = m_is_clv ? "CLV " : "CAV ";
  if (m_is_lead_in)
    string1 += "Lead in ";
  if (m_is_lead_out)
    string1 += "Lead out ";
  if (m_is_stop_code)
    string1 += "Stop ";

  std::string string2 = m_chapter.has_value() ? std::format("Chapter {} ", m_chapter.value()) : "";

  if (m_is_clv) {
    if (m_clv_time_seconds.has_value())
      string2 += std::format("Time {}:{:02}:{:02} ", m_clv_time_seconds.value() / 3600, m_clv_time_seconds.value() % 3600 / 60, m_clv_time_seconds.value() % 60);
    if (m_clv_picture_number.has_value())
      string2 += std::format("Frame {:02} ", m_clv_picture_number.value());
  } else {
    if (m_cav_picture_number.has_value())
      string2 += std::format("Frame {:05} ", m_cav_picture_number.value());
  }

  return {string1, string2};
}

std::optional<double> VbiData::playbackTimeSeconds() const {
  constexpr double ntsc_fps = 30000.0 / 1001.0; // 29.97
  if (m_is_clv) {
    if (!m_clv_time_seconds.has_value()) return std::nullopt;
    double t = static_cast<double>(m_clv_time_seconds.value());
    if (m_clv_picture_number.has_value())
      t += (m_clv_picture_number.value() - 1) / ntsc_fps;
    return t;
  }
  if (m_cav_picture_number.has_value())
    return (m_cav_picture_number.value() - 1) / ntsc_fps;
  return std::nullopt;
}
