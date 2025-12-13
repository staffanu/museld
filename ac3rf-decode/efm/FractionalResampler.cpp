//
// Created by staffanu on 11/15/25.
//

#include <cassert>
#include <cmath>
#include <cstring>
#include "FractionalResampler.h"

FractionalResampler::FractionalResampler(int input_block_size)
    : m_input_block_size(input_block_size),
      m_buffer(nullptr),
      m_prev_data{},
      m_t(0.0),
      m_block_count(0) {
}

void FractionalResampler::updateInput(const float *input) {
    if (m_buffer != nullptr) {
        m_t -= m_input_block_size;
        m_block_count++;
    }
    m_buffer = input;
}

// Returns true if we can safely access data max_look_forward in time after advancing time by dt.
// If false, updateInput needs to be called before calling resample.
bool FractionalResampler::advanceTimeAndResample(double dt, float &output) {
    double new_time = m_t + dt;

    if (new_time + 2.0 >= m_input_block_size) {
        // Returning false will trigger a call to updateInput, but then the input buffer contains new data.
        // Save the end of the input now.
        memcpy(m_prev_data, m_buffer + m_input_block_size - c_max_look_back, c_max_look_back * sizeof(float));
        return false;
    }

    m_t += dt;
    const double t = m_t;

    // Create an interpolating polynomial at for points ix0 ... ix3
    // and evaluate between ix1 and ix2.
    const int ix1 = (int)std::floor(t); // notice could be negative
    const int ix0 = ix1 - 1;
    const int ix2 = ix1 + 1;
    const int ix3 = ix1 + 2;
    assert(ix0 >= -c_max_look_back);

    const float b0 = ix0 >= 0 ? m_buffer[ix0] : m_prev_data[c_max_look_back + ix0];
    const float b1 = ix1 >= 0 ? m_buffer[ix1] : m_prev_data[c_max_look_back + ix1];
    const float b2 = ix2 >= 0 ? m_buffer[ix2] : m_prev_data[c_max_look_back + ix2];
    const float b3 = ix3 >= 0 ? m_buffer[ix3] : m_prev_data[c_max_look_back + ix3];

    double x = 1 + t - ix1;
    // cubic spline though 4 points, f(x) = a0 + a1 x + a2 x(x-1) + a3 x(x-1)(x-2)
    double a0 = b0;
    double a1 = b1 - a0;
    double a2 = (b2 - a0 - 2 * a1) / 2;
    double a3 = (b3 - a0 - 3 * a1 - 6 * a2) / 6;
    // now evaluate at point 1 + p
    output = a0 + a1 * x + a2 * x * (x - 1) + a3 * x * (x - 1) * (x - 2);

    return true;
}

size_t FractionalResampler::get_index() const {
    return (m_block_count - 1) * (size_t)m_input_block_size + (size_t)m_t;
}
