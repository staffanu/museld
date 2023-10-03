//
// Created by staffanu on 4/9/23.
//

#include "FieldBufferView.h"

using namespace std;

FieldBufferView::FieldBufferView(
        Logger &log, int frame_no, MuseBuffer &data, int field_parity)
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

void FieldBufferView::ProcessControlData(uint16_t const *control_data, std::pair<float, float> const &eq) {
    m_control.emplace(ControlSignalDecoder(m_log, control_data, eq));
}

shared_ptr<musevk::VulkanBuffer> FieldBufferView::getVulkanBuffer() {
    return m_data.getVulkanBuffer();
}

optional<ControlSignalDecoder> const &FieldBufferView::control_data() {
    return m_prev_field->m_control;
}
