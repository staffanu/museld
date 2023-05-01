//
// Created by staffanu on 4/9/23.
//

#include <algorithm>
#include <numeric>
#include <iostream>
#include "FrameBuffer.h"
#include "FieldBufferView.h"

using namespace std;

FrameBuffer::FrameBuffer(int frame_no, uint8_t *data) :
        m_frame_no(frame_no),
        m_data(data),
        m_frame_mem(data, 1125, 480),
        m_fields({FieldBufferView(frame_no, data, 0), FieldBufferView(frame_no, data, 1) })
{
}

FrameBuffer::~FrameBuffer() {
    delete[] m_data;
}

uint8_t *FrameBuffer::data() {
    return m_data;
}

pair<double, double> FrameBuffer::estimate_eq() {
    int line_1_high_sum = 0;
    int line_2_low_sum = 0;
    for (int i = 19; i < 259; i++) {
        line_1_high_sum += m_frame_mem(0, i);
        line_2_low_sum += m_frame_mem(1, i);
    }
    int blanking_sum =  0;
    for (int i = 128; i < 384; i++)
        blanking_sum += m_frame_mem(1124, i);

    double avg_high = line_1_high_sum / 240.0;
    double avg_low = line_2_low_sum / 240.0;
    double avg_blanking = blanking_sum / 512.0;
    vector<pair<double, double>> v = {{16.0 , avg_low}, {128.0, avg_blanking }, {239.0, avg_high }};
    auto eq = linear_regression(v);
    return eq;
}

pair<double, double> FrameBuffer::linear_regression(vector<pair<double, double>> const &values) {
    double sum_x = accumulate(values.begin(), values.end(), 0.0,
                              [](double acc, pair<double, double> v) { return acc + v.first; });
    double sum_y = accumulate(values.begin(), values.end(), 0.0,
                              [](double acc, pair<double, double> v) { return acc + v.second; });
    double sum_xx = accumulate(values.begin(), values.end(), 0.0,
                              [](double acc, pair<double, double> v) { return acc + v.first * v.first; });
    double sum_xy = accumulate(values.begin(), values.end(), 0.0,
                              [](double acc, pair<double, double> v) { return acc + v.first * v.second; });
    int n = (int)values.size();
    double a = (n * sum_xy - sum_x * sum_y) / (n * sum_xx - sum_x * sum_x);
    double b = (sum_y - a * sum_x) / n;
    return {a, b};
}

array<uint8_t, 256> FrameBuffer::m_inv_gamma_Y = []() constexpr {
    auto inv_gamma_Y = [](uint8_t y) {
        constexpr uint8_t min_value = 16;
        constexpr uint8_t max_value = 239;
        constexpr uint8_t value_range = 224;
        double normalized = (double)(max(min_value, min(max_value, y)) - min_value) / value_range;
        auto x = (0.6 * normalized + 0.4) * normalized;
        return (uint8_t)(x * value_range + min_value);
    };
    array<uint8_t, 256> arr{};
    for (int i = 0; i < 256; i++)
        arr[i] = inv_gamma_Y(i);
    return arr;
}();

array<uint8_t, 256> FrameBuffer::m_inv_gamma_C = []() constexpr {
    auto inv_gamma_C = [](uint8_t v) {
        double a = -48.0/11.0;
        double b = 67.0/33.0;
        double c = -1.0/132.0;
        double sign = v < 128 ? -1.0 : 1.0;
        int v_magnitude = abs(v - 128);
        double y = min(112, v_magnitude) / 112.0;
        double x = y < 5.0 / 72.0 ?
            0.6 * y :
            y < 47.0 / 33.0 / 8.0 ?
                -b / (2 * a) - sqrt(b * b / (4 * a * a) - (c - y) / a) :
                33.0 / 31.0 * (y - 2.0 / 33.0);
        return (uint8_t)(128 + x * sign * 112);
    };
    array<uint8_t, 256> arr{};
    for (int i = 0; i < 256; i++)
        arr[i] = inv_gamma_C(i);
    return arr;
}();

void FrameBuffer::ApplyInverseTransmissionGamma() {
    for (int field_no = 0; field_no < 2; field_no++) {
        auto buffer_Y = m_fields[field_no].frame_buffer_Y();
        auto buffer_C = m_fields[field_no].frame_buffer_C();
        for (int row = 0; row < MUSE_BUF_HEIGHT; row++) {
            for (int col = 0; col < MUSE_Y_BUF_WIDTH; col++)
                buffer_Y(row, col) = m_inv_gamma_Y[buffer_Y(row, col)];
            for (int col = 0; col < MUSE_C_BUF_WIDTH; col++)
                buffer_C(row, col) = m_inv_gamma_C[buffer_C(row, col)];
        }
    }
}

FieldBufferView &FrameBuffer::get_field(int parity) {
    return m_fields[parity];
}
