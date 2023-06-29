//
// Created by staffanu on 4/9/23.
//

#include <numeric>
#include <utility>
#include <netinet/in.h>
#include "FrameBuffer.h"
#include "FieldBufferView.h"

using namespace std;

FrameBuffer::FrameBuffer(int frame_no, MuseBuffer data)
: m_frame_no(frame_no),
  m_data(std::move(data)),
  m_fields({FieldBufferView(frame_no, m_data, 0), FieldBufferView(frame_no, m_data, 1) }) {
}

void FrameBuffer::set_frame_no(int frame_no) {
    m_frame_no = frame_no;
    m_fields[0].set_frame_no(frame_no);
    m_fields[1].set_frame_no(frame_no);
}

MuseBuffer &FrameBuffer::data() {
    return m_data;
}

std::pair<float, float> FrameBuffer::EstimateEq(uint16_t const *data) {
    int line_1_high_sum = 0;
    int line_2_low_sum = 0;
    for (int i = 19; i < 259; i++) {
        line_1_high_sum += MAYBE_SWAP_BYTE_ORDER(data[0 * MUSE_TOTAL_WIDTH + i]);
        line_2_low_sum += MAYBE_SWAP_BYTE_ORDER(data[1 * MUSE_TOTAL_WIDTH + i]);
    }
    int blanking_sum =  0;
    for (int i = 127; i < 383; i++) {
        blanking_sum += MAYBE_SWAP_BYTE_ORDER(data[562 * MUSE_TOTAL_WIDTH + i]) +
                MAYBE_SWAP_BYTE_ORDER(data[1124 * MUSE_TOTAL_WIDTH + i]);
    }

    float avg_high = (float)line_1_high_sum / MUSE_SHORT_INPUT_MULT / 240.0f;
    float avg_low = (float)line_2_low_sum / MUSE_SHORT_INPUT_MULT/ 240.0f;
    float avg_blanking = (float)blanking_sum / MUSE_SHORT_INPUT_MULT / 512.0f;
    vector<pair<float, float>> v = {{16.0f , avg_low}, {128.0f, avg_blanking }, {239.0f, avg_high }};
    auto eq = FrameBuffer::LinearRegression(v);
    return eq;
}

pair<float, float> FrameBuffer::LinearRegression(vector<pair<float, float>> const &values) {
    float sum_x = accumulate(values.begin(), values.end(), 0.0f,
                              [](float acc, pair<float, float> v) { return acc + v.first; });
    float sum_y = accumulate(values.begin(), values.end(), 0.0f,
                              [](float acc, pair<float, float> v) { return acc + v.second; });
    float sum_xx = accumulate(values.begin(), values.end(), 0.0f,
                              [](float acc, pair<float, float> v) { return acc + v.first * v.first; });
    float sum_xy = accumulate(values.begin(), values.end(), 0.0f,
                              [](float acc, pair<float, float> v) { return acc + v.first * v.second; });
    int n = (int)values.size();
    float a = ((float)n * sum_xy - sum_x * sum_y) / ((float)n * sum_xx - sum_x * sum_x);
    float b = (sum_y - a * sum_x) / (float)n;
    return {a, b};
}

FieldBufferView &FrameBuffer::get_field(int parity) {
    return m_fields[parity];
}

void FrameBuffer::ProcessControlData(uint16_t const *frame_data, std::pair<float, float> const &eq) {
    m_fields[0].ProcessControlData(frame_data + 558 * MUSE_TOTAL_WIDTH + 12, eq);
    m_fields[1].ProcessControlData(frame_data + 1120 * MUSE_TOTAL_WIDTH + 12, eq);
}
