//
// Created by staffanu on 12/10/23.
//

#ifndef MUSECPP_RFDEMODULATOR_H
#define MUSECPP_RFDEMODULATOR_H

#include <fcntl.h>
#include <unistd.h>
#include <fmt/format.h>
#include <cassert>
#include "Logger.h"

class RfDemodulator {
public:
    RfDemodulator(Logger &log, const std::string &filename)
    : m_log(log),
    m_filename(filename),
    m_bytes_per_sample(2),
    m_input_is_fifo(false),
    m_file_input_buffer_bytes(c_input_buffer_lookback),
    m_file_input_buffer_read_pos(c_input_buffer_lookback)
    {
    }

    void demodulate();

private:
//    static constexpr size_t c_sample_buffer_size = 256;
    static constexpr size_t c_input_buffer_size = 655360;
    static constexpr int c_input_buffer_lookback = 3; // we look back 3 samples

    bool readSamples(size_t sample_count, float buffer[], double dt);

    Logger &m_log;
    const std::string m_filename;
    int m_bytes_per_sample;
    const bool m_input_is_fifo;

    int m_file_fd;
    uint8_t m_file_input_buffer[c_input_buffer_size];
    int m_file_input_buffer_bytes;
    int m_file_input_buffer_read_pos;
    double m_t; // fractional part of the time of the previous read (always in [0, 1), counted in input samples)
};


#endif //MUSECPP_RFDEMODULATOR_H
