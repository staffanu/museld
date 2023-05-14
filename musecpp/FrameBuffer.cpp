//
// Created by staffanu on 4/9/23.
//

#include <algorithm>
#include <numeric>
#include "FrameBuffer.h"
#include "FieldBufferView.h"

using namespace std;

FrameBuffer::FrameBuffer(int frame_no, std::shared_ptr<MuseBuffer<float>> const &data)
: m_frame_no(frame_no),
  m_data(data),
  m_fields({FieldBufferView(frame_no, data, 0), FieldBufferView(frame_no, data, 1) })
{
}

std::shared_ptr<MuseBuffer<float>> FrameBuffer::data() {
    return m_data;
}

pair<float, float> FrameBuffer::estimate_eq() {
    float line_1_high_sum = 0;
    float line_2_low_sum = 0;
    for (int i = 19; i < 259; i++) {
        line_1_high_sum += (*m_data)[0][i];
        line_2_low_sum += (*m_data)[1][i];
    }
    float blanking_sum =  0;
    for (int i = 128; i < 384; i++)
        blanking_sum += (*m_data)[1124][i];

    float avg_high = line_1_high_sum / 240.0f;
    float avg_low = line_2_low_sum / 240.0f;
    float avg_blanking = blanking_sum / 512.0f;
    vector<pair<float, float>> v = {{16.0f , avg_low}, {128.0f, avg_blanking }, {239.0f, avg_high }};
    auto eq = linear_regression(v);
    return eq;
}

pair<float, float> FrameBuffer::linear_regression(vector<pair<float, float>> const &values) {
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

void FrameBuffer::ApplyInverseTransmissionGamma() {
    Shaders::GetInstance().ApplyTransmissionGamma(*m_data);
}

FieldBufferView &FrameBuffer::get_field(int parity) {
    return m_fields[parity];
}
