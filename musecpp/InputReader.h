//
// Created by staffanu on 6/25/23.
//

#ifndef MUSECPP_INPUTREADER_H
#define MUSECPP_INPUTREADER_H

#include <cstdint>
#include <fstream>

class InputReader {
public:
    explicit InputReader(const std::string &filename)
    : m_filename(filename) {};

    [[nodiscard]] bool initialize();

    // Reads 480 * 1125 unsigned shorts into the given buffer, corresponding to a MUSE frame
    bool readShorts(uint16_t *buffer) {
        return readShorts(m_input, buffer, 480 * 1125);
    }


private:
    bool readBigEndianToBuffer(std::ifstream &input, float *out, size_t n);
    static bool readShorts(std::ifstream &input, uint16_t *out, size_t n);
    std::pair<int, std::pair<float, float>> compute_initial_skip();

    std::string m_filename;
    std::ifstream m_input;
};

#endif //MUSECPP_INPUTREADER_H
