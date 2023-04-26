//
// Created by staffanu on 4/9/23.
//

#include <algorithm>
#include "FrameBuffer.h"
#include "FieldBufferView.h"

using namespace std;

FrameBuffer::FrameBuffer(int frame_no, DecoderInt *data) :
    _frame_no(frame_no),
    _data(data),
    _frame_mem(data, 1125, 480),
    _fields({FieldBufferView(frame_no, data, 0), FieldBufferView(frame_no, data, 1) })
{
}

FrameBuffer::~FrameBuffer() {
    delete[] _data;
}

DecoderInt *FrameBuffer::data() {
    return _data;
}

std::pair<double, double> FrameBuffer::estimate_eq() {
    return { 0.37, 285.0 };
}

void FrameBuffer::ApplyInverseTransmissionGamma() {
    auto inv_gamma_Y = [](DecoderInt y) {
        constexpr DecoderInt min_value = 16 * MUSE_MULT;
        constexpr DecoderInt max_value = 239 * MUSE_MULT;
        constexpr DecoderInt value_range = 224 * MUSE_MULT;
        double normY = (double)(max(min_value, min(max_value, y)) - min_value) / value_range;
        auto x = 0.6 * normY * normY + 0.4 * normY;
        DecoderInt inv_gamma = x * value_range + min_value;
        return inv_gamma;
    };

    for (int field_no = 0; field_no < 2; field_no++) {
        auto buffer_Y = _fields[field_no].frame_buffer_Y();
        for (int row = 0; row < MUSE_Y_BUF_HEIGHT; row++) {
            for (int col = 0; col < MUSE_Y_BUF_WIDTH; col++) {
                buffer_Y(row, col) = inv_gamma_Y(buffer_Y(row, col));
            }
        }
    }
}

FieldBufferView &FrameBuffer::get_field(int parity) {
    return _fields[parity];
}
