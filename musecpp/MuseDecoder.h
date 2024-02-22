//
// Created by staffanu on 5/22/23.
//

#ifndef MUSECPP_MUSEDECODER_H
#define MUSECPP_MUSEDECODER_H

#include <deque>
#include "AudioDecoder.h"
#include "musevk/TimestampStatistics.h"
#include "efm/EfmDecoder.h"

namespace musevk {
    class TimestampQueryPool;
}
class AudioDecoder;
class FrameBuffer;
class InputReader;
class Logger;
class Shaders;

class MuseDecoder {
public:
    /// \param decode_all_fields If false, skips decoding of the first field of each
    /// frame individually, so next should be called 30 times per second instead of 60;
    /// useful for slow hardware.
    MuseDecoder(Logger &log,
                InputReader &reader,
                Shaders &shaders,
                musevk::VulkanManager &manager,
                bool decode_video,
                bool decode_all_fields,
                bool decode_audio,
                musevk::TimestampQueryPool *timestamp_query_pool);
    ~MuseDecoder();
    MuseDecoder(const MuseDecoder&) = delete;
    void operator=(const MuseDecoder&) = delete;

    [[nodiscard]] bool initialize();

    enum FieldInterpolationMode {
        eNormal, eForceIntraField, eForceInterFrame
    };

    bool next(bool efm_audio, AudioMode &audio_mode,
              size_t &sample_count,
              AudioDecoder::AudioFrame output_samples[AudioDecoder::c_max_output_samples],
              FieldInterpolationMode field_interpolation_mode,
              bool redo_last_field, bool enable_non_linear, bool enable_dropout_compensation);

    void output_benchmark_results();

private:
    Logger &m_log;
    InputReader &m_reader;
    Shaders &m_shaders;
    musevk::VulkanManager &m_manager;
    const bool m_decode_video;
    const bool m_decode_all_fields;
    const bool m_decode_audio;
    musevk::TimestampQueryPool *m_timestamp_query_pool; // if set we use it

    std::pair<float, float> m_eq;
    vk::Semaphore m_first_stage_complete_semaphore;
    std::shared_ptr<musevk::CommandBuffer> m_reset_timestamp_query_pool_command_buffer;
    std::shared_ptr<musevk::CommandBuffer> m_first_stage_command_buffer;
    std::shared_ptr<musevk::CommandBuffer> m_second_stage_command_buffer;
    musevk::TimestampStatistics m_timestamp_statistics;
    int m_frame_no;
    int m_field_index; // 0 if a new frame needs to be read, 1 when we should process the second field
    long m_total_elapsed_time_us;
    AudioDecoder m_audio_decoder;
    EfmDecoder m_efm_decoder;
    std::deque<FrameBuffer *> m_frame_buffers; // The front (index 0) is the newest received frame
};


#endif //MUSECPP_MUSEDECODER_H
