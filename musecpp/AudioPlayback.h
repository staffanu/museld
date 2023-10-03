//
// Created by staffanu on 6/21/23.
//

#ifndef MUSECPP_AUDIOPLAYBACK_H
#define MUSECPP_AUDIOPLAYBACK_H

#include <atomic>
#include <portaudio.h>
#include "AudioDecoder.h"

class Logger;

class AudioPlayback {
public:
    AudioPlayback(Logger &log);
    void add_samples(
            AudioDecoder::AudioMode &audio_mode,
            size_t &sample_count,
            AudioDecoder::AudioFrame output_samples[AudioDecoder::c_max_output_samples]);
    void cleanup();

private:
    static constexpr int c_audio_buffer_size = 8192;
    static constexpr int c_audio_buffer_optimal_filled = 4096;
    static constexpr double c_audio_buffer_speed_adjust_constant = 1e-6;
    static constexpr double c_audio_buffer_max_speed_adjust = 0.01;

    Logger &m_log;
    AudioDecoder::AudioFrame m_audio_buffer[c_audio_buffer_size];
    std::atomic<int> m_next_audio_buffer_write_ix;
    std::atomic<int> m_next_audio_buffer_read_ix;
    std::atomic<double> m_audio_speed_adjust; // m_audio_buffer to skip per sample -- 0.1 skips every 10th sample
    double m_audio_speed_adjust_sum;
    AudioDecoder::AudioMode m_current_mode;
    int m_channels_used;
    PaStream *m_audio_stream;

    static int audio_callback(const void *input_buffer, void *output_buffer, unsigned long frames_per_buffer,
                              const PaStreamCallbackTimeInfo *time_info, PaStreamCallbackFlags status_flags,
                              void *userData);

    int audioCallbackMember(void *output_buffer, unsigned long frames_per_buffer,
                            const PaStreamCallbackTimeInfo *time_info, PaStreamCallbackFlags status_flags);

    void openStream();
    void closeStream();
};

#endif //MUSECPP_AUDIOPLAYBACK_H
