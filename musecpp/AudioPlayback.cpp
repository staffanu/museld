//
// Created by staffanu on 6/21/23.
//

#include <cassert>
#include "AudioPlayback.h"

using namespace std;

int AudioPlayback::audio_callback(const void *input_buffer, void *output_buffer, unsigned long frames_per_buffer,
                                  const PaStreamCallbackTimeInfo *time_info, PaStreamCallbackFlags status_flags,
                                  void *userData) {
    return ((AudioPlayback *)userData)->audioCallbackMember(output_buffer, frames_per_buffer, time_info, status_flags);
}

AudioPlayback::AudioPlayback()
: m_audio_buffer{},
  m_next_audio_buffer_write_ix(0),
  m_next_audio_buffer_read_ix(0),
  m_audio_speed_adjust(0),
  m_audio_speed_adjust_sum(0),
  m_current_mode(AudioDecoder::MODE_UNKNOWN),
  m_audio_stream(nullptr) {
    auto audio_status = Pa_Initialize();
    if (audio_status != paNoError)
        throw runtime_error(Pa_GetErrorText(audio_status));
}

void AudioPlayback::cleanup() {
    if (m_audio_stream != nullptr)
        closeStream();

    auto audio_status = Pa_Terminate();
    if (audio_status != paNoError)
        throw runtime_error(Pa_GetErrorText(audio_status));
}

void AudioPlayback::add_samples(AudioDecoder::AudioMode &audio_mode, size_t &sample_count,
                                AudioDecoder::AudioFrame *output_samples) {
    assert(audio_mode != AudioDecoder::MODE_UNKNOWN);
    if (audio_mode != m_current_mode) {
        m_current_mode = audio_mode;
        if (m_audio_stream != nullptr)
            closeStream();
        m_next_audio_buffer_write_ix = 0;
        m_next_audio_buffer_read_ix = 0;
        openStream();
    }

    if (m_next_audio_buffer_write_ix == 0 && m_next_audio_buffer_read_ix == 0)
        m_audio_speed_adjust = 0;
    else { // do not adjust speed when starting
        int audio_buffer_size = m_next_audio_buffer_write_ix >= m_next_audio_buffer_read_ix ?
                                m_next_audio_buffer_write_ix - m_next_audio_buffer_read_ix :
                                c_audio_buffer_size - m_next_audio_buffer_read_ix + m_next_audio_buffer_write_ix;
        if (audio_buffer_size < c_audio_buffer_optimal_filled - c_audio_buffer_filled_flexibility)
            m_audio_speed_adjust = max(
                    (audio_buffer_size - (c_audio_buffer_optimal_filled - c_audio_buffer_filled_flexibility)) *
                    c_audio_buffer_speed_adjust_constant, -c_audio_buffer_max_speed_adjust);
        else if (audio_buffer_size > c_audio_buffer_optimal_filled + c_audio_buffer_filled_flexibility)
            m_audio_speed_adjust = min(
                    (audio_buffer_size - (c_audio_buffer_optimal_filled + c_audio_buffer_filled_flexibility)) *
                    c_audio_buffer_speed_adjust_constant, c_audio_buffer_max_speed_adjust);
        else
            m_audio_speed_adjust = 0;

        cout << "Audio buffer size: " << audio_buffer_size << ", adjustment: " << m_audio_speed_adjust << endl;
    }

    for (int i = 0; i < sample_count; i++) {
        auto updated_write_ix = m_next_audio_buffer_write_ix + 1;
        if (updated_write_ix == c_audio_buffer_size)
            updated_write_ix = 0;
        if (updated_write_ix != m_next_audio_buffer_read_ix) {
            m_audio_buffer[m_next_audio_buffer_write_ix] = output_samples[i];
            m_next_audio_buffer_write_ix = updated_write_ix;
        }
    }
}

int AudioPlayback::audioCallbackMember(void *output_buffer, unsigned long frames_per_buffer,
                                       const PaStreamCallbackTimeInfo *time_info, PaStreamCallbackFlags status_flags) {
    auto *out = (int16_t *)output_buffer;
    unsigned int i;

    for (i = 0; i < frames_per_buffer; i++) {
        if (m_next_audio_buffer_read_ix != m_next_audio_buffer_write_ix) {
            auto frame = m_audio_buffer[m_next_audio_buffer_read_ix];
            m_audio_speed_adjust_sum += m_audio_speed_adjust + 1.0;
            while (m_audio_speed_adjust_sum >= 1.0) {
                m_next_audio_buffer_read_ix++;
                if (m_next_audio_buffer_read_ix == c_audio_buffer_size)
                    m_next_audio_buffer_read_ix = 0;
                m_audio_speed_adjust_sum -= 1.0;
            }
            *out++ = frame.channel1; // TODO: figure out which channel is which
            *out++ = frame.channel2;
            if (m_current_mode == AudioDecoder::MODE_A) {
                *out++ = frame.channel3;
                *out++ = frame.channel4;
            }
        } else {
            *out++ = 0;
            *out++ = 0;
            if (m_current_mode == AudioDecoder::MODE_A) {
                *out++ = 0;
                *out++ = 0;
            }
        }
    }
    return 0;
}

void AudioPlayback::openStream() {
    auto audio_status = Pa_OpenDefaultStream(&m_audio_stream,
                                        0, // no input channels
                                        m_current_mode == AudioDecoder::MODE_A ? 4 : 2,
                                        paInt16,
                                        m_current_mode == AudioDecoder::MODE_A ? 32000 : 48000,
                                        256, // frames per buffer, maybe use paFramesPerBufferUnspecified
                                        audio_callback,
                                        this);
    if (audio_status != paNoError)
        throw runtime_error(Pa_GetErrorText(audio_status));

    audio_status = Pa_StartStream(m_audio_stream);
    if (audio_status != paNoError)
        throw runtime_error(Pa_GetErrorText(audio_status));
}

void AudioPlayback::closeStream() {
    auto audio_status = Pa_AbortStream(m_audio_stream);
    if (audio_status != paNoError)
        throw runtime_error(Pa_GetErrorText(audio_status));

    audio_status = Pa_CloseStream(m_audio_stream);
    if (audio_status != paNoError)
        throw runtime_error(Pa_GetErrorText(audio_status));
}
