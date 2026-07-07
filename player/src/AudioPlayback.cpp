//
// Created by staffanu on 6/21/23.
//

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#include <miniaudio.h>

#include <cassert>
#include <format>
#include "AudioPlayback.h"
#include "logging/Logger.h"

using namespace std;

namespace {

// Semantic meaning of AudioFrame::samples[k]; miniaudio routes/mixes these to
// whatever channel layout the output device actually has.
constexpr ma_channel c_channel_map[4] = {
    MA_CHANNEL_FRONT_LEFT,  // channel 1
    MA_CHANNEL_FRONT_RIGHT, // channel 2
    MA_CHANNEL_BACK_LEFT,   // channel 3
    MA_CHANNEL_BACK_RIGHT,  // channel 4
};

} // namespace

void AudioPlayback::audio_callback(ma_device *device, void *output_buffer, const void *input_buffer,
                                   unsigned int frames_per_buffer) {
    (void)input_buffer;
    ((AudioPlayback *)device->pUserData)->audioCallbackMember(output_buffer, frames_per_buffer);
}

AudioPlayback::AudioPlayback(Logger &log)
: m_log(log),
  m_audio_buffer{},
  m_next_audio_buffer_write_ix(0),
  m_next_audio_buffer_read_ix(0),
  m_audio_speed_adjust(0),
  m_audio_speed_adjust_sum(0),
  m_current_mode(MODE_UNKNOWN),
  m_channels_used(0),
  m_context(nullptr),
  m_device(nullptr) {
    m_context = new ma_context;
    auto audio_status = ma_context_init(nullptr, 0, nullptr, m_context);
    if (audio_status != MA_SUCCESS) {
        delete m_context;
        m_context = nullptr;
        throw runtime_error(string("miniaudio: ") + ma_result_description(audio_status));
    }

    m_log.debug(eAudio, std::format("Audio backend: {}", ma_get_backend_name(m_context->backend)));
}

void AudioPlayback::cleanup() {
    if (m_device != nullptr)
        closeStream();

    if (m_context != nullptr) {
        auto audio_status = ma_context_uninit(m_context);
        delete m_context;
        m_context = nullptr;
        if (audio_status != MA_SUCCESS)
            throw runtime_error(string("miniaudio: ") + ma_result_description(audio_status));
    }
}

void AudioPlayback::add_samples(AudioMode const &audio_mode, int const &sample_count,
                                AudioFrame *output_samples) {
    assert(audio_mode != MODE_UNKNOWN);
    if (audio_mode != m_current_mode) {
        m_current_mode = audio_mode;
        if (m_device != nullptr)
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

        m_log.debug(eAudio, std::format("Audio buffer size: {}, adjustment: {:.5f}",
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
        m_log.error(eAudio, std::format("Discarded {} samples", discard_count));
}

void AudioPlayback::audioCallbackMember(void *output_buffer, unsigned long frames_per_buffer) {
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
                *out++ = frame.samples[k]; // in c_channel_map order
        } else {
            underrun_count++;
            for (int k = 0; k < m_channels_used; k++)
                *out++ = 0;
        }
    }
    if (underrun_count)
        m_log.info(eAudio, std::format("Audio buffer underrun: {} missing samples", underrun_count));
}

void AudioPlayback::openStream() {
    ma_device_info *device_infos;
    ma_uint32 device_count;
    auto audio_status = ma_context_get_devices(m_context, &device_infos, &device_count, nullptr, nullptr);
    if (audio_status == MA_SUCCESS)
        for (ma_uint32 i = 0; i < device_count; i++)
            m_log.debug(eAudio, std::format("Device {}: {}{}", i, device_infos[i].name,
                                            device_infos[i].isDefault ? " (default)" : ""));

    m_channels_used = m_current_mode == MODE_A ? 4 : 2;

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_s16;
    config.playback.channels = m_channels_used;
    config.playback.pChannelMap = const_cast<ma_channel *>(c_channel_map);
    config.sampleRate = m_current_mode == MODE_A ? 32000 : m_current_mode == MODE_B ? 48000 : 44100;
    config.periodSizeInFrames = 256;
    config.dataCallback = audio_callback;
    config.pUserData = this;

    m_device = new ma_device;
    audio_status = ma_device_init(m_context, &config, m_device);
    if (audio_status != MA_SUCCESS) {
        delete m_device;
        m_device = nullptr;
        throw runtime_error(string("miniaudio: ") + ma_result_description(audio_status));
    }

    m_log.info(eAudio, std::format("Using device {}: {} channels at {} Hz (device native: {} channels at {} Hz)",
                                   m_device->playback.name, m_channels_used, config.sampleRate,
                                   m_device->playback.internalChannels, m_device->playback.internalSampleRate));

    audio_status = ma_device_start(m_device);
    if (audio_status != MA_SUCCESS)
        throw runtime_error(string("miniaudio: ") + ma_result_description(audio_status));
}

void AudioPlayback::closeStream() {
    auto audio_status = ma_device_stop(m_device);
    ma_device_uninit(m_device);
    delete m_device;
    m_device = nullptr;
    if (audio_status != MA_SUCCESS && audio_status != MA_DEVICE_NOT_STARTED)
        throw runtime_error(string("miniaudio: ") + ma_result_description(audio_status));
}
