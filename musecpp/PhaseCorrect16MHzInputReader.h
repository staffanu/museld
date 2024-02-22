//
// Created by staffanu on 6/30/23.
//

#ifndef MUSECPP_PHASECORRECT16MHZINPUTREADER_H
#define MUSECPP_PHASECORRECT16MHZINPUTREADER_H

#include <fstream>
#include "InputReader.h"

class PhaseCorrect16MHzInputReader : public InputReader {
public:
    explicit PhaseCorrect16MHzInputReader(Logger &log, const std::string &filename, bool big_endian,
                                          double initial_seek_seconds,
                                          const std::optional<std::string> &output_filename);

    bool initialize(std::vector<std::unique_ptr<InputReader::InputReaderBlock>> &buffers) override;
    void cleanup() override;

    void seek(double seconds) override;

protected:
    void threadFunc() override;

private:
    bool readFloats(std::ifstream &input, float *out, size_t n);
    std::pair<int, std::pair<float, float>> compute_initial_skip(Logger &log);

    std::ifstream m_input;
    bool m_big_endian;
};

#endif //MUSECPP_PHASECORRECT16MHZINPUTREADER_H
