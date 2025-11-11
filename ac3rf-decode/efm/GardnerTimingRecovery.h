//
// Created by staffanu on 11/11/25.
//

#ifndef AC3RF_DECODE_GARDNERTIMINGRECOVERY_H
#define AC3RF_DECODE_GARDNERTIMINGRECOVERY_H

#include <cassert>
#include <cmath>
#include <cstring>

#include "../Logger.h"

class FractionalResampler {
public:
    explicit FractionalResampler(int input_block_size)
      : m_input_block_size(input_block_size),
        m_buffer(nullptr),
        m_prev_data{},
        m_t(0.0) {
    }

    void updateInput(const float *input) {
        if (m_buffer != nullptr)
            memcpy(m_prev_data, m_buffer + m_input_block_size - c_max_look_back, c_max_look_back * sizeof(float));
        m_buffer = input;
        m_t -= m_input_block_size;
    }

    // returns true if we can safely access data max_look_forward in time after advancing time by dt.
    // If false, updateInput needs to be called before calling resample.
    bool advanceTime(double dt, double max_look_forward) {
        m_t += dt;
        return m_t + max_look_forward + 2.0 < m_input_block_size;
    }

    float resample(double dt) {
        assert(m_t - dt - 2.0 > -c_max_look_back);

        // Create an interpolating polynomial at for points ix0 ... ix3
        // and evaluate between ix1 and ix2.
        const int ix1 = (int)std::floor(m_t); // notice could be negative
        const int ix0 = ix1 - 1;
        const int ix2 = ix1 + 1;
        const int ix3 = ix1 + 2;
        assert(ix0 >= -c_max_look_back);

        const float b0 = ix0 >= 0 ? m_buffer[ix0] : m_prev_data[c_max_look_back + ix0];
        const float b1 = ix1 >= 0 ? m_buffer[ix1] : m_prev_data[c_max_look_back + ix1];
        const float b2 = ix2 >= 0 ? m_buffer[ix2] : m_prev_data[c_max_look_back + ix2];
        const float b3 = ix3 >= 0 ? m_buffer[ix3] : m_prev_data[c_max_look_back + ix3];

        double x = 1 + m_t - ix1;
        // cubic spline though 4 points, f(x) = a0 + a1 x + a2 x(x-1) + a3 x(x-1)(x-2)
        double a0 = b0;
        double a1 = b1 - a0;
        double a2 = (b2 - a0 - 2 * a1) / 2;
        double a3 = (b3 - a0 - 3 * a1 - 6 * a2) / 6;
        // now evaluate at point 1 + p
        double y = a0 + a1 * x + a2 * x * (x - 1) + a3 * x * (x - 1) * (x - 2);

        return (float)y;
    }

private:
    static constexpr int c_max_look_back = 10;

    int m_input_block_size;
    const float *m_buffer;
    float m_prev_data[c_max_look_back];
    double m_t;
};

class GardnerTimingRecovery {
public:
    GardnerTimingRecovery(Logger &log, double input_sample_frequency, int input_block_size);

    GardnerTimingRecovery(const GardnerTimingRecovery &) = delete;
    GardnerTimingRecovery &operator=(const GardnerTimingRecovery &) = delete;
    GardnerTimingRecovery(GardnerTimingRecovery &&) = delete;
    GardnerTimingRecovery &operator=(GardnerTimingRecovery &&) = delete;


    int reclock(const float *input, bool *output, int max_output_size);

private:
    Logger &m_log;
    double m_input_sample_frequency;
    int m_input_block_size;
    FractionalResampler m_resampler;
    double m_nominal_step_size;
    double m_step_size_adjustment;
    double m_error_sum;
};

#endif //AC3RF_DECODE_GARDNERTIMINGRECOVERY_H