//
// Created by staffanu on 6/30/23.
//

#ifndef MUSECPP_PHASECORRECT16MHZFRAMEREADER_H
#define MUSECPP_PHASECORRECT16MHZFRAMEREADER_H

#include <fstream>
#include "FrameReader.h"
#include "muse/MuseInputBlock.h"

class PhaseCorrect16MHzFrameReader : public FrameReader<MuseInputBlock> {
public:
    explicit PhaseCorrect16MHzFrameReader(Logger &log, const std::string &filename, bool big_endian,
                                          double initial_seek_seconds,
                                          const std::optional<std::string> &output_filename);

    bool initialize(std::vector<std::unique_ptr<MuseInputBlock>> &buffers) override;
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

#endif //MUSECPP_PHASECORRECT16MHZFRAMEREADER_H
