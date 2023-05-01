//
// Created by staffanu on 4/9/23.
//

#include "Eigen/Dense"
#include "MuseTypes.h"
#include "FieldBufferView.h"
#include "ControlSignalDecoder.h"

using namespace std;

FieldBufferView::FieldBufferView(int frame_no, uint8_t *data, int field_parity):
        m_frame_no(frame_no),
        m_data(data),
        m_field_parity(field_parity) {
}

void FieldBufferView::ProcessControlData(MappedFrameMatrix const &control_data) {
    m_control = optional(ControlSignalDecoder(control_data));
}

MappedFrameMatrix FieldBufferView::control_data_buffer() {
    return {m_data + 480 * (m_field_parity == 0 ? 558 : 1120) + 12,
            5, 94,
            FRAME_STRIDE };
}

MappedFrameMatrix FieldBufferView::frame_buffer_Y() {
    return {m_data + 480 * (m_field_parity == 0 ? 46 : 608) + 106,
            MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH,
            FRAME_STRIDE };
}

MappedFrameMatrix FieldBufferView::frame_buffer_C() {
    return {m_data + 480 * (m_field_parity == 0 ? 42 : 604) + 11,
            MUSE_BUF_HEIGHT, MUSE_C_BUF_WIDTH,
            FRAME_STRIDE };
}

MappedFrameMatrix FieldBufferView::audio_buffer() {
    return {m_data + 480 * (m_field_parity == 0 ? 2 : 564) + 11,
            44, 469,
            FRAME_STRIDE };
}
