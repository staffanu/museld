//
// Created by staffanu on 5/22/23.
//

#ifndef MUSECPP_MUSEDECODER_H
#define MUSECPP_MUSEDECODER_H

#include <deque>
#include "Shaders.h"
#include "AudioDecoder.h"
#include "musevk/TimestampStatistics.h"
#include "InputReader.h"
#include "Logger.h"

class FrameBuffer;

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
                bool benchmark_shaders);
    ~MuseDecoder();
    [[nodiscard]] bool initialize();

    enum FieldInterpolationMode {
        eNormal, eForceIntraField, eForceInterFrame
    };

    bool next(AudioDecoder::AudioMode &audio_mode,
              size_t &sample_count,
              AudioDecoder::AudioFrame output_samples[AudioDecoder::c_max_output_samples],
              FieldInterpolationMode field_interpolation_mode,
              bool redo_last_field);

private:
    Logger &m_log;
    InputReader &m_reader;
    Shaders &m_shaders;
    musevk::VulkanManager &m_manager;
    const bool m_decode_video;
    const bool m_decode_all_fields;
    const bool m_decode_audio;
    const bool m_benchmark_shaders;

    std::pair<float, float> m_eq;
    std::shared_ptr<musevk::CommandQueue> m_command_queue;
    musevk::TimestampStatistics m_timestamp_statistics;
    int m_frame_no;
    int m_field_index; // 0 if a new frame needs to be read, 1 when we should process the second field
    long m_total_elapsed_time_us;
    AudioDecoder m_audio_decoder;
    std::deque<FrameBuffer *> m_frame_buffers; // The front (index 0) is the newest received frame
};


#endif //MUSECPP_MUSEDECODER_H
