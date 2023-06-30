//
// Created by staffanu on 6/30/23.
//

#ifndef MUSECPP_RESAMPLINGINPUTREADER_H
#define MUSECPP_RESAMPLINGINPUTREADER_H

#include "InputReader.h"

class ResamplingInputReader : public InputReader {
public:
    explicit ResamplingInputReader(const std::string &filename, int sample_rate, bool stop_on_eof);

    bool initialize(std::vector<std::shared_ptr<musevk::VulkanBuffer>> const &buffers) override;
    void cleanup() override;

protected:
    void threadFunc() override;

private:
    static constexpr size_t c_sample_buffer_size = 480;
    static constexpr size_t c_input_buffer_size = 1024 * 1024;
    static constexpr int c_input_buffer_min_read_pos = 3; // we look back 3 bytes

    bool readSamples(int sample_count, uint16_t buffer[c_sample_buffer_size], double dt);

    InputPll m_input_pll;
    int m_file_fd;
    uint8_t m_file_input_buffer[c_input_buffer_size];
    int m_file_input_buffer_bytes;
    int m_file_input_buffer_read_pos;
    double m_t; // the time of the previous read
};

#endif //MUSECPP_RESAMPLINGINPUTREADER_H
