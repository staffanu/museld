//
// Created by staffanu on 6/25/23.
//

#ifndef MUSECPP_INPUTREADER_H
#define MUSECPP_INPUTREADER_H

#include <cstdint>
#include <fstream>
#include <cmath>
#include <queue>
#include <optional>
#include "InputPll.h"

class InputReader {
public:
    explicit InputReader(const std::string &filename, int sample_rate, bool stop_on_eof)
    : m_filename(filename),
      m_sample_rate(sample_rate),
      m_stop_on_eof(stop_on_eof),
      m_input_pll(sample_rate),
      m_input{},
      m_file_fd(-1),
      m_exit_flag(false),
      m_input_buffer{},
      m_t(0),
      m_interpolation_buffer{},
      m_input_buffer_bytes(0),
      m_input_buffer_read_pos(0),
      m_last_written_ix(0) {
    };

    [[nodiscard]] bool initialize();

    ~InputReader();

        // Reads 480 * 1125 unsigned shorts into the given buffer, corresponding to a MUSE frame
    bool readShorts(uint16_t *buffer);
//    {
//        return readShorts(m_input, buffer, 480 * 1125);
//    }

private:
    uint16_t readSample(double dt);

    // We keep a total of c_interpolation_buffer_size values in the input queue for interpolation
    static constexpr size_t c_input_buffer_size = 16384;
    static constexpr int c_interpolation_buffer_size = 4;

    bool readBigEndianToBuffer(std::ifstream &input, float *out, size_t n);
    static bool readShorts(std::ifstream &input, uint16_t *out, size_t n);
    std::pair<int, std::pair<float, float>> compute_initial_skip();

    std::string m_filename;
    int m_sample_rate;
    bool m_stop_on_eof;
    InputPll m_input_pll;
    std::ifstream m_input;
    int m_file_fd;
    bool m_exit_flag;
    uint8_t m_input_buffer[c_input_buffer_size];
    int m_input_buffer_bytes;
    int m_input_buffer_read_pos;

    double m_t; // the time of the previous read
    uint8_t m_interpolation_buffer[c_interpolation_buffer_size];
    int m_last_written_ix;
};

#endif //MUSECPP_INPUTREADER_H
