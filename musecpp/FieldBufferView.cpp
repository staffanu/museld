//
// Created by staffanu on 4/9/23.
//

#include "MuseTypes.h"
#include "FieldBufferView.h"
#include "ControlSignalDecoder.h"
#include "MuseSubBuffer.h"

using namespace std;

FieldBufferView::FieldBufferView(
        int frame_no, std::shared_ptr<MuseBuffer<float>> const &data, int field_parity)
: m_frame_no(frame_no),
  m_data(data),
  m_field_parity(field_parity) {
}

void FieldBufferView::ProcessControlData(MuseSubBuffer const &control_data) {
    m_control = optional(ControlSignalDecoder(control_data));
}

std::shared_ptr<kp::Tensor> FieldBufferView::tensor() {
    return m_data->tensor();
}

MuseSubBuffer FieldBufferView::control_data_buffer() const {
    return {m_data, m_field_parity == 0 ? 558u : 1120u, 12, 5, 94 };
}

MuseSubBuffer FieldBufferView::audio_buffer() const {
    return {m_data, m_field_parity ? 2u : 564u, 11, 44, 469 };
}
