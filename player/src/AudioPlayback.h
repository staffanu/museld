// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#ifndef MUSECPP_AUDIOPLAYBACK_H
#define MUSECPP_AUDIOPLAYBACK_H

#include <atomic>
#include "AudioDefs.h"
#include "muse/AudioDecoder.h"

class Logger;
struct ma_context;
struct ma_device;

class AudioPlayback {
public:
    explicit AudioPlayback(Logger &log);
    void add_samples(
            AudioMode const &audio_mode,
            int const &sample_count,
            AudioFrame output_samples[MAX_AUDIO_OUTPUT_SAMPLES]);
    void cleanup();

private:
    static constexpr int c_audio_buffer_size = 16384;
    static constexpr int c_audio_buffer_optimal_filled = 4096;
    static constexpr double c_speed_adjust_p_gain = 2e-6;  // per frame of buffer level error
    static constexpr double c_speed_adjust_i_gain = 1e-8;  // per frame of error, per add_samples call
    static constexpr double c_max_speed_adjust = 0.01;

    Logger &m_log;
    AudioFrame m_audio_buffer[c_audio_buffer_size];
    std::atomic<int> m_next_audio_buffer_write_ix;
    std::atomic<int> m_next_audio_buffer_read_ix;
    std::atomic<double> m_audio_speed_adjust; // consume-rate offset -- 0.1 consumes 1.1 frames per output frame
    std::atomic<bool> m_prebuffering; // output silence until the buffer reaches the optimal fill level
    std::atomic<int> m_underrun_count; // underrun events since last add_samples call
    double m_speed_adjust_integral; // producer thread only
    double m_read_frac; // fractional part of the read position, callback thread only
    AudioMode m_current_mode;
    int m_channels_used;
    ma_context *m_context;
    ma_device *m_device;

    static void audio_callback(ma_device *device, void *output_buffer, const void *input_buffer,
                               unsigned int frames_per_buffer);

    void audioCallbackMember(void *output_buffer, unsigned long frames_per_buffer);

    void openStream();
    void closeStream();
};

#endif //MUSECPP_AUDIOPLAYBACK_H
