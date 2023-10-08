//
// Created by staffanu on 6/21/23.
//

#include <cassert>
#include <fmt/format.h>
#include "AudioPlayback.h"
#include "Logger.h"

using namespace std;

int AudioPlayback::audio_callback(const void *input_buffer, void *output_buffer, unsigned long frames_per_buffer,
                                  const PaStreamCallbackTimeInfo *time_info, PaStreamCallbackFlags status_flags,
                                  void *userData) {
    return ((AudioPlayback *)userData)->audioCallbackMember(output_buffer, frames_per_buffer, time_info, status_flags);
}

AudioPlayback::AudioPlayback(Logger &log)
: m_log(log),
  m_audio_buffer{},
  m_next_audio_buffer_write_ix(0),
  m_next_audio_buffer_read_ix(0),
  m_audio_speed_adjust(0),
  m_audio_speed_adjust_sum(0),
  m_current_mode(AudioDecoder::MODE_UNKNOWN),
  m_audio_stream(nullptr) {
    auto audio_status = Pa_Initialize();
    if (audio_status != paNoError)
        throw runtime_error(string("Portaudio: ") + Pa_GetErrorText(audio_status));

    PaHostApiIndex count = Pa_GetHostApiCount();
    for (PaHostApiIndex i = 0; i < count; i++) {
        auto *info = Pa_GetHostApiInfo(i);
        m_log.debug(eAudio, fmt::format("Host API {}: type id={}, device ocunt={}, default device={}",
                                        info->name, (int)info->type, info->deviceCount, (int)info->defaultOutputDevice));
    }
}

void AudioPlayback::cleanup() {
    if (m_audio_stream != nullptr)
        closeStream();

    auto audio_status = Pa_Terminate();
    if (audio_status != paNoError)
        throw runtime_error(string("Portaudio: ") + Pa_GetErrorText(audio_status));
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
        if (audio_buffer_size < c_audio_buffer_optimal_filled)
            m_audio_speed_adjust = max(
                    (audio_buffer_size - c_audio_buffer_optimal_filled) * c_audio_buffer_speed_adjust_constant,
                    -c_audio_buffer_max_speed_adjust);
        else if (audio_buffer_size > c_audio_buffer_optimal_filled)
            m_audio_speed_adjust = min(
                    (audio_buffer_size - c_audio_buffer_optimal_filled) * c_audio_buffer_speed_adjust_constant,
                    c_audio_buffer_max_speed_adjust);
        else
            m_audio_speed_adjust = 0;

        m_log.debug(eAudio, fmt::format("Audio buffer size: {}, adjustment: {:.5f}",
                                        audio_buffer_size, (double)m_audio_speed_adjust));
    }

    int discard_count = 0;
    for (int i = 0; i < sample_count; i++) {
        auto updated_write_ix = m_next_audio_buffer_write_ix + 1;
        if (updated_write_ix == c_audio_buffer_size)
            updated_write_ix = 0;
        if (updated_write_ix != m_next_audio_buffer_read_ix) {
            m_audio_buffer[m_next_audio_buffer_write_ix] = output_samples[i];
            m_next_audio_buffer_write_ix = updated_write_ix;
        } else
            discard_count++;
    }
    if (discard_count != 0)
        m_log.error(eAudio, fmt::format("Discarded {} samples", discard_count));
}

int AudioPlayback::audioCallbackMember(void *output_buffer, unsigned long frames_per_buffer,
                                       const PaStreamCallbackTimeInfo *time_info, PaStreamCallbackFlags status_flags) {
    auto *out = (int16_t *)output_buffer;
    unsigned int i;
    int underrun_count = 0;

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
            for (int k = 0; k < m_channels_used; k++)
                *out++ = frame.samples[k]; // TODO: figure out which channel is which
        } else {
            underrun_count++;
            for (int k = 0; k < m_channels_used; k++)
                *out++ = 0;
        }
    }
    if (underrun_count)
        m_log.error(eAudio, fmt::format("Audio buffer underrun: {} missing samples", underrun_count));
    return 0;
}

void AudioPlayback::openStream() {
    PaDeviceIndex count = Pa_GetDeviceCount();
    for (PaDeviceIndex i = 0; i < count; i++) {
        auto *info = Pa_GetDeviceInfo(i);
        m_log.debug(eAudio, fmt::format("Device {}: maximum {} output channels",
                                        info->name, info->maxOutputChannels));
    }

    PaDeviceIndex device_index = Pa_GetDefaultOutputDevice();
    auto *info = Pa_GetDeviceInfo(device_index);
    m_channels_used = min(m_current_mode == AudioDecoder::MODE_A ? 4 : 2, info->maxOutputChannels);
    m_log.info(eAudio, fmt::format("Using device {}: maximum {} output channels, {} used.",
                                   info->name, info->maxOutputChannels, m_channels_used));

    PaStreamParameters parameters {
        device_index,
        m_channels_used,
        paInt16,
        info->defaultLowOutputLatency,
        nullptr};

    auto audio_status = Pa_OpenStream(&m_audio_stream,
                                      nullptr, // no input channels
                                      &parameters,
                                      m_current_mode == AudioDecoder::MODE_A ? 32000.0 : 48000.0,
                                      256ul, // frames per buffer, maybe use paFramesPerBufferUnspecified
                                      paNoFlag, // stream flags
                                      audio_callback,
                                      this);
    if (audio_status != paNoError)
        throw runtime_error(string("Portaudio: ") + Pa_GetErrorText(audio_status));

    audio_status = Pa_StartStream(m_audio_stream);
    if (audio_status != paNoError)
        throw runtime_error(string("Portaudio: ") + Pa_GetErrorText(audio_status));
}

void AudioPlayback::closeStream() {
    auto audio_status = Pa_AbortStream(m_audio_stream);
    if (audio_status != paNoError)
        throw runtime_error(string("Portaudio: ") + Pa_GetErrorText(audio_status));

    audio_status = Pa_CloseStream(m_audio_stream);
    if (audio_status != paNoError)
        throw runtime_error(string("Portaudio: ") + Pa_GetErrorText(audio_status));
}
