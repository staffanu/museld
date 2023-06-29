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
    explicit InputReader(const std::string &filename, int sample_rate, bool stop_on_eof);

    [[nodiscard]] bool initialize();

    ~InputReader();

        // Reads 480 * 1125 unsigned shorts into the given buffer, corresponding to a MUSE frame
    bool readShorts(uint16_t *buffer);
//    {
//        return readShorts(m_input, buffer, 480 * 1125);
//    }

private:
    // We keep a total of c_interpolation_buffer_size values in the input queue for interpolation
    static constexpr size_t c_sample_buffer_size = 480;
    static constexpr size_t c_input_buffer_size = 1024 * 1024;
    static constexpr int c_input_buffer_min_read_pos = 3; // we look back 3 bytes

    bool readSamples(int sample_count, uint16_t buffer[c_sample_buffer_size], double dt);

    bool readBigEndianToBuffer(std::ifstream &input, float *out, size_t n);
    static bool readShorts(std::ifstream &input, uint16_t *out, size_t n);
    std::pair<int, std::pair<float, float>> compute_initial_skip();

    std::string m_filename;
    bool m_stop_on_eof;
    InputPll m_input_pll;
    std::ifstream m_input;
    int m_file_fd;
    uint8_t m_input_buffer[c_input_buffer_size];
    int m_input_buffer_bytes;
    int m_input_buffer_read_pos;

    double m_t; // the time of the previous read
};

#endif //MUSECPP_INPUTREADER_H
