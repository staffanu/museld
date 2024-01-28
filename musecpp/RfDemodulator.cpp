//
// Created by staffanu on 12/10/23.
//

#include <cmath>
#include <thread>
#include <complex>
#include <vector>
#include <iostream>
#include "RfDemodulator.h"

using namespace std;


// This is faster than doing mod -- use the test code below to make sure
// we only ever call with 0 <= i <= 2 * n - 1
#define MOD(i, n) (i < n ? i : i - n)

//int MOD(int i, int n) {
//    if (i < 0)
//        throw runtime_error("less than zero");
//    else if (i < n)
//        return i;
//    else if (i - n < n)
//        return i - n;
//    else
//        throw runtime_error("overflow");
//}

vector<complex<double>> bandpass_filter = {
        complex<double>(-0.022242598235607147, -0.016160201281309128),
        complex<double>(-0.006259475834667683, 0.019264668226242065),
        complex<double>(-0.0653596818447113, -1.9738074286124174e-08),
        complex<double>(-0.018518002703785896, -0.05699261650443077),
        complex<double>(-0.01667506992816925, 0.012115138582885265),
        complex<double>(-0.12461019307374954, -0.09053468704223633),
        complex<double>(0.08611813187599182, -0.2650439739227295),
        complex<double>(0.3294519782066345, 1.5709494505244948e-07),
        complex<double>(0.08611787855625153, 0.26504403352737427),
        complex<double>(-0.1246102824807167, 0.09053456783294678),
        complex<double>(-0.016675058752298355, -0.012115154415369034),
        complex<double>(-0.018518056720495224, 0.05699259787797928),
        complex<double>(-0.0653596818447113, -4.259377561766087e-08),
        complex<double>(-0.00625945720821619, -0.019264673814177513),
        complex<double>(-0.02224261313676834, 0.016160178929567337),
};

vector<double> lowpass_filter = {
        0.017518598586320877,
        0.03721041977405548,
        0.024483997374773026,
        -0.018038881942629814,
        -0.058205537497997284,
        -0.05336624011397362,
        0.018355419859290123,
        0.13716724514961243,
        0.2481795698404312,
        0.29339081048965454,
        0.2481795698404312,
        0.13716724514961243,
        0.018355419859290123,
        -0.05336624011397362,
        -0.058205537497997284,
        -0.018038881942629814,
        0.024483997374773026,
        0.03721041977405548,
        0.017518598586320877,
};

// Notice that the root raised cosine filter works on half the sample rate, i.e., 31.25 MHz
vector<double> rrc_filter = vector<double>{
    0.048089, 0.028415, -0.092251, -0.029997, 0.296034, 0.499418, 0.296034, -0.029997, -0.092251, 0.028415, 0.048089
};

bool RfDemodulator::initialize() {
    m_input_fd = open(m_filename.c_str(), O_NONBLOCK);
    if (m_input_fd == -1)
        throw runtime_error("Unable to open file");

#ifdef linux
    if (m_input_is_fifo) {
        m_log.debug(eInput, fmt::format("Pipe size: {}", fcntl(m_input_fd, F_GETPIPE_SZ)));
        fcntl(m_input_fd, F_SETPIPE_SZ, 1024 * 1024);
        m_log.debug(eInput, fmt::format("Pipe size now: {}", fcntl(m_input_fd, F_GETPIPE_SZ)));
    }
#endif

    return true;
}

void RfDemodulator::cleanup() {
    close(m_input_fd);
}

void RfDemodulator::demodulate() {
    auto m_out_file_fd = open("outfile",  O_WRONLY | O_TRUNC | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (m_out_file_fd == -1)
        throw runtime_error("Unable to open output file");

    const int decimation_rate = 2;

    vector<double> input_buffer;
    input_buffer.resize(c_input_buffer_size);

    int bandpass_filter_size = (int)bandpass_filter.size();
    int bandpass_half_size = bandpass_filter_size / 2;
    assert(bandpass_half_size * 2 + 1 == bandpass_filter_size); // we expect an odd number of coefficients

    int lowpass_filter_size = (int)lowpass_filter.size();
    assert(lowpass_filter_size / 2 * 2 + 1 == lowpass_filter_size);
    vector<double> lowpass_buffer;
    lowpass_buffer.resize(lowpass_filter_size);
    int lowpass_buffer_write_ix = 0;

    int rrc_filter_size = (int)rrc_filter.size();
    assert(rrc_filter_size / 2 * 2 + 1 == rrc_filter_size);
    vector<double> rrc_buffer;
    rrc_buffer.resize(rrc_filter_size);
    int rrc_buffer_write_ix = 0;

    int output_buffer_size = (c_input_buffer_size - 2 * bandpass_half_size) / decimation_rate;
    int16_t output_buffer[output_buffer_size];

    double prev_angle;
    long sample_counter = 0;
    auto t0 = chrono::high_resolution_clock::now();

    if (!readDoubles(input_buffer.data(), bandpass_half_size * 2))
        throw runtime_error("input file too short");

    while (readDoubles(input_buffer.data() + bandpass_half_size * 2, c_input_buffer_size - bandpass_half_size * 2) && sample_counter < 62500000 * 2) {
        int output_index = 0;
        for (int index = bandpass_half_size * 2; index < c_input_buffer_size; index++) {
            sample_counter++;

            complex<double> analytic = 0;
            for (int i = 0; i < bandpass_filter_size; i++)
                analytic += bandpass_filter[i] * input_buffer[index - i];

            double angle = arg(analytic);
            double phase_diff = angle > prev_angle ? angle - prev_angle : 2 * M_PI + angle - prev_angle;
            prev_angle = angle;

            double fm_out = (phase_diff * (62.5e6 / (2 * M_PI * 12.5e6)) - 1) * (12.5e6 / 1.9e6);

            lowpass_buffer[lowpass_buffer_write_ix++] = fm_out;
            if (lowpass_buffer_write_ix == lowpass_filter_size)
                lowpass_buffer_write_ix = 0;

//            if ((sample_counter & 1) == 0) {
            if (sample_counter % decimation_rate == 0) {
                double low_pass_sum = 0;
                for (int i = 0; i < lowpass_filter_size; i++)
                    low_pass_sum += lowpass_filter[i] * lowpass_buffer[MOD(lowpass_buffer_write_ix + i, lowpass_filter_size)];

                rrc_buffer[rrc_buffer_write_ix] = low_pass_sum;
                rrc_buffer_write_ix++;
                if (rrc_buffer_write_ix == rrc_filter_size)
                    rrc_buffer_write_ix = 0;

                double rrc_sum = 0;
                for (int i = 0; i < rrc_filter_size; i++)
                    rrc_sum += rrc_filter[i] * rrc_buffer[MOD(rrc_buffer_write_ix + i, rrc_filter_size)];

                auto value = (int16_t) clamp(rrc_sum * 15000.0, -32768.0, 32767.0);
                output_buffer[output_index++] = value;
            }
        }
        assert(output_index = output_buffer_size);
        if (write(m_out_file_fd, output_buffer, output_buffer_size * sizeof(int16_t)) != output_buffer_size * sizeof(int16_t))
            throw runtime_error("write failed");

        for (int i = 0; i < bandpass_filter_size - 1; i++)
            input_buffer[i] = input_buffer[c_input_buffer_size - bandpass_filter_size + 1 + i];
    }

    auto t1 = chrono::high_resolution_clock::now();
    auto time_us = (double) chrono::duration_cast<chrono::microseconds>(t1 - t0).count();
    m_log.info(ePerformance, fmt::format("Total time {:.3f} s", time_us / 1e6));
}

bool RfDemodulator::readDoubles(double *out, size_t n) {
    int filled_bytes = 0;
    do {
        ssize_t read_count = read(m_input_fd,
                                  (void *)((char *)m_tmp_input_buffer + filled_bytes),
                                  n * sizeof(int16_t) - filled_bytes);
        if (read_count == -1 && errno == EAGAIN)
            this_thread::sleep_for(chrono::milliseconds(1));
        else if (read_count == 0) {
            if (!m_input_is_fifo)
                return false;
        } else if (read_count == -1)
            throw runtime_error(fmt::format("Error reading from file: {}", strerror(errno)));
        else
            filled_bytes += (int)read_count;
    } while (filled_bytes < n * sizeof(int16_t));

    for (int i = 0; i < n; i++)
        out[i] = (double)m_tmp_input_buffer[i];

    return true;
}
