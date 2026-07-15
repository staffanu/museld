// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "NtscFieldView.h"

NtscFieldView::NtscFieldView(Logger &log, int frame_no, const std::shared_ptr<musevk::VulkanBuffer> &data,
    const std::shared_ptr<musevk::VulkanBuffer> &burst_phase_data,
    const std::shared_ptr<musevk::VulkanBuffer> &y_data, const std::shared_ptr<musevk::VulkanBuffer> &c_data,
    int field_parity)
: m_log(log),
  m_frame_no(frame_no),
  m_data(data),
  m_burst_phase_data(burst_phase_data),
  m_y_data(y_data),
  m_c_data(c_data),
  m_field_parity(field_parity),
  m_prev_field(nullptr) {
}
