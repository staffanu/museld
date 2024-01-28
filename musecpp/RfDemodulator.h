//
// Created by staffanu on 12/10/23.
//

#ifndef MUSECPP_RFDEMODULATOR_H
#define MUSECPP_RFDEMODULATOR_H

#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <fmt/format.h>
#include <cassert>
#include "Logger.h"

class RfDemodulator {
public:
    RfDemodulator(Logger &log, const std::string &filename)
    : m_log(log),
    m_filename(filename),
    m_input_fd(-1),
    m_input_is_fifo(false)
    {
    }

    bool initialize();
    void demodulate();
    void cleanup();

private:
    static constexpr int c_input_buffer_size = 32768;

    bool readDoubles(double *out, size_t n);

    Logger &m_log;
    const std::string m_filename;
    int m_input_fd;
    const bool m_input_is_fifo;
    int16_t m_tmp_input_buffer[c_input_buffer_size];
};


#endif //MUSECPP_RFDEMODULATOR_H
