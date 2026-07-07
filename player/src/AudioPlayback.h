//
// Created by staffanu on 6/21/23.
//

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
    static constexpr double c_audio_buffer_speed_adjust_constant = 2e-6;
    static constexpr double c_audio_buffer_max_speed_adjust = 0.01;

    Logger &m_log;
    AudioFrame m_audio_buffer[c_audio_buffer_size];
    std::atomic<int> m_next_audio_buffer_write_ix;
    std::atomic<int> m_next_audio_buffer_read_ix;
    std::atomic<double> m_audio_speed_adjust; // m_audio_buffer to skip per sample -- 0.1 skips every 10th sample
    double m_audio_speed_adjust_sum;
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
