//
// Created by staffanu on 6/22/24.
//

#include "NtscFieldView.h"

NtscFieldView::NtscFieldView(Logger &log, int frame_no, const std::shared_ptr<musevk::VulkanBuffer> &data,
    const std::shared_ptr<musevk::VulkanBuffer> &y_data, const std::shared_ptr<musevk::VulkanBuffer> &c_data,
    int field_parity)
: m_log(log),
  m_frame_no(frame_no),
  m_data(data),
  m_y_data(data),
  m_c_data(data),
  m_field_parity(field_parity),
  m_prev_field(nullptr) {
}
