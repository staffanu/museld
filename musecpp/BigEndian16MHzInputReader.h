//
// Created by staffanu on 6/30/23.
//

#ifndef MUSECPP_BIGENDIAN16MHZINPUTREADER_H
#define MUSECPP_BIGENDIAN16MHZINPUTREADER_H

#include <fstream>
#include "InputReader.h"

class BigEndian16MHzInputReader : public InputReader {
public:
    explicit BigEndian16MHzInputReader(Logger &log, const std::string &filename,
                                       bool input_is_fifo, double initial_seek_seconds);

    bool initialize(std::vector<std::shared_ptr<musevk::VulkanBuffer>> const &buffers) override;
    void cleanup() override;

protected:
    void threadFunc() override;

private:
    bool readBigEndianToBuffer(std::ifstream &input, float *out, size_t n);
    static bool readShorts(std::ifstream &input, uint16_t *out, size_t n);
    std::pair<int, std::pair<float, float>> compute_initial_skip();

    std::ifstream m_input;
};

#endif //MUSECPP_BIGENDIAN16MHZINPUTREADER_H
