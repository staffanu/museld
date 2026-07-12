// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#include "FieldBufferView.h"

using namespace std;

FieldBufferView::FieldBufferView(
        Logger &log, int frame_no, shared_ptr<musevk::VulkanBuffer> const &data, int field_parity)
: m_log(log),
  m_frame_no(frame_no),
  m_data(data),
  m_field_parity(field_parity),
  m_control(nullopt),
  m_prev_field(nullptr) {
}

void FieldBufferView::set_frame_no(int frame_no) {
    m_frame_no = frame_no;
}

void FieldBufferView::set_prev_field(FieldBufferView *prev_field) {
    m_prev_field = prev_field;
}

void FieldBufferView::ProcessControlData(float const *control_data, std::pair<float, float> const &rescale) {
    m_control.emplace(ControlSignalDecoder(m_log, control_data, rescale));
}

shared_ptr<musevk::VulkanBuffer> FieldBufferView::getVulkanBuffer() {
    return m_data;
}

optional<ControlSignalDecoder> const &FieldBufferView::control_data() const {
    return m_prev_field->m_control;
}

std::optional<ControlSignalDecoder> const &FieldBufferView::own_control_data() const {
    return m_control;
}
