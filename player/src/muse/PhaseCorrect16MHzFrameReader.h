// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

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
    // Join the reader thread while threadFunc() and m_input_reader still
    // exist; the base destructor's cleanup() would be too late.
    ~PhaseCorrect16MHzFrameReader() override {
        PhaseCorrect16MHzFrameReader::cleanup();
    }

    bool initialize(std::vector<std::unique_ptr<MuseInputBlock>> &buffers) override;
    void cleanup() override;

    void seek(double seconds) override;

protected:
    void threadFunc() override;

private:
    std::pair<int, std::pair<float, float>> compute_initial_skip(Logger &log);

    InputFormat m_input_format;
    std::unique_ptr<InputReader> m_input_reader;
    std::pair<float, float> m_input_scale = {0.f, 0.f};
    // Input-reader position in frames past the sync origin found by
    // compute_initial_skip().  Seeks are clamped here: position 0 is the first
    // start-of-frame in the capture, which need not be the start of the file.
    off_t m_frame_position = 0;
};

#endif //MUSECPP_PHASECORRECT16MHZFRAMEREADER_H
