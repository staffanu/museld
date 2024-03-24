//
// Created by staffanu on 6/30/23.
//

#ifndef MUSECPP_RESAMPLINGINPUTREADER_H
#define MUSECPP_RESAMPLINGINPUTREADER_H

#include "InputReader.h"
#include "RfDemodulator.h"
#include "efm/EfmPll.h"

class ResamplingInputReader : public InputReader {
public:
    enum InputFormat {
        eUnsignedByte,
        eSignedShortLittleEndian,
        eFloat,
    };
    explicit ResamplingInputReader(Logger &log, const std::string &executable_dir, musevk::VulkanManager &vulkan_manager,
                                   const std::string &filename, InputFormat input_format,
                                   double sample_rate, double initial_seek_seconds,
                                   bool demodulate, bool benchmark_shaders,
                                   const std::optional<std::string> &output_filename);
    ResamplingInputReader(const ResamplingInputReader&) = delete;
    void operator=(const ResamplingInputReader&) = delete;

    bool initialize(std::vector<std::unique_ptr<InputReader::InputReaderBlock>> &buffers) override;
    void cleanup() override;
    void seek(double seconds) override;

protected:
    void threadFunc() override;

private:
    static constexpr size_t c_sample_buffer_size = 480;
    static constexpr size_t c_input_buffer_size = DemodulatedBlock::c_video_block_size * sizeof(float) + 100;
    static constexpr int c_input_buffer_lookback = 3; // we look back 3 samples

    bool readSamples(int sample_count, float buffer[c_sample_buffer_size],
                     uint8_t dropout_buffer[c_sample_buffer_size], double dt,
                     float efm_buffer[DemodulatedBlock::c_efm_block_size],
                     bool *have_efm, long *sample_buffer_input_offset, int *source_samples_per_sample);

    InputFormat m_input_format;
    InputPll m_input_pll;
    EfmPll m_efm_pll;
    int m_file_fd;
    double m_sample_rate;

    RfDemodulator *m_demodulator;

    uint8_t m_file_input_buffer[c_input_buffer_size];
    int m_file_input_buffer_bytes;
    int m_file_input_buffer_read_pos;
    long m_file_input_buffer_input_offset;
    uint8_t m_dropout_input_buffer[c_input_buffer_size]{};
    double m_t; // the time of the previous read
    int m_bytes_per_sample;
    double m_output_multiplier;
    double m_output_add;
};

#endif //MUSECPP_RESAMPLINGINPUTREADER_H
