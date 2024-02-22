//
// Created by staffanu on 12/10/23.
//

#include <thread>
#include <complex>
#include <vector>
#include <iostream>
#include "RfDemodulator.h"

using namespace std;

vector<complex<float>> band_pass_filter = {
#include "band_pass_filter.h"
};

size_t c_sample_buffer_size = 16384; // (256 - 79) + band_pass_filter.size();

//vector<float> low_pass_filter = {
//#include "low_pass_filter.h"
//};

void RfDemodulator::demodulate() {
    m_file_fd = open(m_filename.c_str(), O_NONBLOCK);
    if (m_file_fd == -1)
        throw runtime_error("Unable to open file");

    auto m_out_file_fd = open("outfile",  O_WRONLY | O_TRUNC | O_CREAT,
                              S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (m_out_file_fd == -1)
        throw runtime_error("Unable to open output file");

    float buffer[c_sample_buffer_size];
    int band_pass_filter_size = band_pass_filter.size();
    int band_pass_half_size = band_pass_filter_size / 2;
    assert(band_pass_half_size * 2 + 1 == band_pass_filter_size); // we expect an odd number of coefficients
    assert(c_sample_buffer_size > band_pass_filter_size + 10);

    int low_pass_filter_size = low_pass_filter.size();
    int half_low_pass_filter_size = low_pass_filter_size / 2;
    assert(half_low_pass_filter_size * 2 + 1 == low_pass_filter_size);
    float low_pass_buffer[low_pass_filter_size];
    int low_pass_buffer_write_ix = 0;
    for (int i = 0; i < low_pass_filter_size; i++)
        low_pass_buffer[i] = 0;

    if (!readSamples(band_pass_half_size * 2, buffer, 1.0))
        throw runtime_error("input file too short");
    complex<float> prev_sum = 0;

    double u0, u1, u2 = 0;
    double r = 0.999;
    double pilot_frequency = 2.278125e6;
    double alpha = -(1 + r) * cos(2 * M_PI * pilot_frequency / 62.5e6);
    double prev_filtered = 0;
    long total_samples = 0;
    int pilot_counter = 0;
    double pilot_frac = 0;
    double cum_error = 0;
    double dt = 1.0;

    while (readSamples(c_sample_buffer_size - band_pass_half_size * 2, buffer + band_pass_half_size * 2, dt)) {
        for (int index = band_pass_half_size * 2; index < c_sample_buffer_size; index++) {
            total_samples++;

            complex<float> sum = 0;
            for (int i = 0; i < band_pass_filter_size; i++)
                sum += band_pass_filter[i] * buffer[index - i];

            auto arg1 = arg(sum * conj(prev_sum));
            prev_sum = sum;

            double band_pass_out = (12.5 / 1.9) * (arg1 * 62.5e6 / (2 * M_PI * 12.5e6) - 1);
            low_pass_buffer[low_pass_buffer_write_ix] = band_pass_out;
            low_pass_buffer_write_ix++;
            if (low_pass_buffer_write_ix == low_pass_filter_size)
                low_pass_buffer_write_ix = 0;

            float low_pass_sum = 0;
            for (int i = 0; i < low_pass_filter_size; i++)
                low_pass_sum += low_pass_filter[i] * low_pass_buffer[(low_pass_buffer_write_ix + i) % low_pass_filter_size];

            // inverse notch filter
            u0 = buffer[index] - alpha * u1 - r * u2;
            double y = (u0 - u2) * (1 - r) / 2;
            u2 = u1;
            u1 = u0;

            if (y < 0 && prev_filtered >= 0) {
                auto samples_since_crossing = -y / (prev_filtered - y);
                auto total_pilot_cycle_samples = pilot_frac + (1 - samples_since_crossing) + pilot_counter - 1;
                double nominal_pilot_cycle_samples = 62.5e6 / pilot_frequency;
                auto deviation = total_samples > 1000 ? total_pilot_cycle_samples - nominal_pilot_cycle_samples : 0;
                assert(abs(deviation) < 1.0);

                cum_error += deviation;
                dt = 1 + cum_error * 10e-4 + deviation * 10e-3;

                //cout << "total_pilot_cycle_samples=" << total_pilot_cycle_samples << ", cum_error = " << cum_error << ", dt=" << dt << endl;

                pilot_frac = samples_since_crossing;
                pilot_counter = 0;

                //low_pass_sum = -20000;
            }
            prev_filtered = y;
            pilot_counter++;

            if (total_samples % (62500000L / 60) == 0) {
                cout << "cum_error = " << cum_error << ", dt=" << dt << endl;
            }

            auto float_value = clamp(low_pass_sum * 32000.f, -32768.0f, 32767.0f);
            auto value = (int16_t)float_value;
            if (write(m_out_file_fd, &value, 2) != 2)
                throw runtime_error("write failed");
        }

        for (int i = 0; i < band_pass_filter_size - 1; i++)
            buffer[i] = buffer[c_sample_buffer_size - band_pass_filter_size + 1 + i];
    }
}

bool RfDemodulator::readSamples(size_t sample_count, float buffer[], double dt) {
    double t = m_t; // always in [0, 1)

    int read_pos = m_file_input_buffer_read_pos;
    for (int sample_ix = 0; sample_ix < sample_count; sample_ix++) {
        t += dt;
        int samples_to_read = (int)t;
        t -= samples_to_read;

        if (read_pos + samples_to_read * m_bytes_per_sample >= m_file_input_buffer_bytes) {
            // move remaining input to start of buffer
            int start_copy_pos = read_pos - c_input_buffer_lookback * m_bytes_per_sample;
            int n = m_file_input_buffer_bytes - start_copy_pos;
            if (n > 0) // 0 first time, and could be later if we have short reads
                memcpy(m_file_input_buffer, m_file_input_buffer + start_copy_pos, n);
            m_file_input_buffer_bytes -= start_copy_pos;
            read_pos = c_input_buffer_lookback * m_bytes_per_sample;

            do {
                ssize_t read_count = read(m_file_fd,
                                          (void *) (m_file_input_buffer + m_file_input_buffer_bytes),
                                          c_input_buffer_size - m_file_input_buffer_bytes);
                if (read_count == -1 && errno == EAGAIN)
                    this_thread::sleep_for(chrono::milliseconds(1));
                else if (read_count == 0) {
                    if (!m_input_is_fifo)
                        return false;
                } else if (read_count == -1)
                    throw runtime_error(fmt::format("Error reading from file: {}", strerror(errno)));
                else
                    m_file_input_buffer_bytes += (int)read_count;
            } while (read_pos + samples_to_read * m_bytes_per_sample >= m_file_input_buffer_bytes);
        }
        read_pos += samples_to_read * m_bytes_per_sample;

        double b0, b1, b2, b3;
        switch (m_bytes_per_sample) {
            case 1:
                b0 = m_file_input_buffer[read_pos - 3] * 256.0;
                b1 = m_file_input_buffer[read_pos - 2] * 256.0;
                b2 = m_file_input_buffer[read_pos - 1] * 256.0;
                b3 = m_file_input_buffer[read_pos - 0] * 256.0;
                break;
            case 2:
                b0 = ((int16_t *)(m_file_input_buffer + read_pos))[-3];
                b1 = ((int16_t *)(m_file_input_buffer + read_pos))[-2];
                b2 = ((int16_t *)(m_file_input_buffer + read_pos))[-1];
                b3 = ((int16_t *)(m_file_input_buffer + read_pos))[0];
                break;
            default:
                throw runtime_error("Unrecognized input format");
        }

        double x = 1 + t;
        // cubic spline though 4 points, f(x) = a0 + a1 x + a2 x(x-1) + a3 x(x-1)(x-2)
        double a0 = b0;
        double a1 = b1 - a0;
        double a2 = (b2 - a0 - 2 * a1) / 2;
        double a3 = (b3 - a0 - 3 * a1 - 6 * a2) / 6;
        // now evaluate at point 1 + p
        double y = a0 + a1 * x + a2 * x * (x - 1) + a3 * x * (x - 1) * (x - 2);

        buffer[sample_ix] = (float)y;
    }
    m_t = t;
    m_file_input_buffer_read_pos = read_pos;
    return true;
}
