//
// Created by staffanu on 6/30/23.
//

#ifndef MUSECPP_RESAMPLINGINPUTREADER_H
#define MUSECPP_RESAMPLINGINPUTREADER_H

#include "InputReader.h"

class ResamplingInputReader : public InputReader {
public:
    enum InputFormat {
        eUnsignedByte,
        eSignedShortLittleEndian,
    };
    explicit ResamplingInputReader(Logger &log, const std::string &filename, InputFormat input_format,
                                   double sample_rate, bool input_is_fifo, double initial_seek_seconds);

    bool initialize(std::vector<std::shared_ptr<musevk::VulkanBuffer>> const &buffers) override;
    void cleanup() override;

protected:
    void threadFunc() override;

private:
    static constexpr size_t c_sample_buffer_size = 480;
    static constexpr size_t c_input_buffer_size = 1024 * 256;
    static constexpr int c_input_buffer_lookback = 3; // we look back 3 samples

    bool readSamples(int sample_count, uint16_t buffer[c_sample_buffer_size], double dt);

    InputFormat m_input_format;
    bool m_input_is_fifo;
    InputPll m_input_pll;
    int m_file_fd;
    uint8_t m_file_input_buffer[c_input_buffer_size];
    int m_file_input_buffer_bytes;
    int m_file_input_buffer_read_pos;
    double m_t; // the time of the previous read
    int m_bytes_per_sample;
    double m_output_multiplier;
    double m_output_add;
};

#endif //MUSECPP_RESAMPLINGINPUTREADER_H
