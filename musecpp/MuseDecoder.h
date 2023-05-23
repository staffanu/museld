//
// Created by staffanu on 5/22/23.
//

#ifndef MUSECPP_MUSEDECODER_H
#define MUSECPP_MUSEDECODER_H

#include <string>
#include <optional>
#include <opencv2/core/mat.hpp>
#include <deque>
#include "Shaders.h"
#include "FrameBuffer.h"
#include "AudioDecoder.h"

class MuseDecoder {
public:
    MuseDecoder(const std::string_view &filename, bool read_floats);
    ~MuseDecoder();
    bool Initialize();
    std::optional<cv::Mat> Next(bool output_bgr);

private:
    std::string_view m_filename;
    bool m_read_floats;
    std::ifstream m_input;
    std::pair<float, float> m_eq;
    uint16_t *m_input_buffer;
    int m_frame_no;
    int m_field_index; // 0 if a new frame needs to be read, 1 when we should process the second field
    long m_total_elapsed_time_us;
    Shaders &m_shaders;
    AudioDecoder m_audio_decoder;
    std::deque<FrameBuffer *> m_frame_buffers;

    bool read_big_endian_to_buffer(std::ifstream &input, float *out, size_t n, std::pair<float, float> eq);
    std::pair<int, std::pair<float, float>> compute_initial_skip();
};


#endif //MUSECPP_MUSEDECODER_H
