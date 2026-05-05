//
// Created by staffanu on 6/30/23.
//

#ifndef MUSECPP_PHASECORRECT16MHZFRAMEREADER_H
#define MUSECPP_PHASECORRECT16MHZFRAMEREADER_H

#include <memory>
#include "FrameReader.h"
#include "input/InputReader.h"
#include "muse/MuseInputBlock.h"

class PhaseCorrect16MHzFrameReader : public FrameReader<MuseInputBlock> {
public:
    explicit PhaseCorrect16MHzFrameReader(Logger &log, const std::string &filename, InputFormat input_format,
                                          double initial_seek_seconds,
                                          const std::optional<std::string> &output_filename);

    bool initialize(std::vector<std::unique_ptr<MuseInputBlock>> &buffers) override;
    void cleanup() override;

    void seek(double seconds) override;

protected:
    void threadFunc() override;

private:
    std::pair<int, std::pair<float, float>> compute_initial_skip(Logger &log);

    InputFormat m_input_format;
    std::unique_ptr<InputReader> m_input_reader;
};

#endif //MUSECPP_PHASECORRECT16MHZFRAMEREADER_H
