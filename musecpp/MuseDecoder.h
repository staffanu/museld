//
// Created by staffanu on 5/22/23.
//

#ifndef MUSECPP_MUSEDECODER_H
#define MUSECPP_MUSEDECODER_H

#include <string>
#include <optional>
#include <deque>
#include <fstream>
#include "Shaders.h"
#include "AudioDecoder.h"
#include "musevk/TimestampStatistics.h"

class FrameBuffer;

class MuseDecoder {
public:
    MuseDecoder(const std::string &filename,
                Shaders &shaders,
                musevk::VulkanManager &manager,
                bool decode_video,
                bool decode_audio,
                bool benchmark_shaders);
    ~MuseDecoder();
    bool Initialize();
    bool Next(AudioDecoder::AudioMode &audio_mode,
              size_t &sample_count,
              AudioDecoder::AudioFrame output_samples[AudioDecoder::c_max_output_samples]);

private:
    std::string m_filename;
    Shaders &m_shaders;
    musevk::VulkanManager &m_manager;
    const bool m_decode_video;
    const bool m_decode_audio;
    const bool m_benchmark_shaders;

    std::ifstream m_input;
    std::pair<float, float> m_eq;
    std::shared_ptr<musevk::VulkanBuffer> m_input_vulkan_buffer;
    std::shared_ptr<musevk::CommandQueue> m_command_queue;
    musevk::TimestampStatistics m_timestamp_statistics;
    int m_frame_no;
    int m_field_index; // 0 if a new frame needs to be read, 1 when we should process the second field
    long m_total_elapsed_time_us;
    AudioDecoder m_audio_decoder;
    std::deque<FrameBuffer *> m_frame_buffers;

    bool read_big_endian_to_buffer(std::ifstream &input, float *out, size_t n);
    static bool read_shorts(std::ifstream &input, uint16_t *out, size_t n);
    std::pair<int, std::pair<float, float>> compute_initial_skip();
};


#endif //MUSECPP_MUSEDECODER_H
