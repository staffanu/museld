// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#include <miniaudio.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <format>
#include "AudioPlayback.h"
#include "logging/Logger.h"
#include "util/Interpolate.h"

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
  m_prebuffering(true),
  m_underrun_count(0),
  m_speed_adjust_integral(0),
  m_read_frac(0),
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
        m_audio_speed_adjust = 0;
        m_speed_adjust_integral = 0;
        m_read_frac = 0; // safe: the stream is closed, no callback is running
        m_prebuffering = true;
        openStream();
    }

    int underrun_count = m_underrun_count.exchange(0);
    if (underrun_count != 0)
        m_log.info(eAudio, std::format("Audio buffer underrun: playback paused to re-buffer ({} occurrences)",
                                       underrun_count));

    int audio_buffer_size = m_next_audio_buffer_write_ix >= m_next_audio_buffer_read_ix ?
                            m_next_audio_buffer_write_ix - m_next_audio_buffer_read_ix :
                            c_audio_buffer_size - m_next_audio_buffer_read_ix + m_next_audio_buffer_write_ix;
    if (m_prebuffering)
        m_audio_speed_adjust = 0; // nothing is being consumed; avoid integrator windup
    else {
        double error = audio_buffer_size - c_audio_buffer_optimal_filled;
        m_speed_adjust_integral = clamp(m_speed_adjust_integral + error * c_speed_adjust_i_gain,
                                        -c_max_speed_adjust, c_max_speed_adjust);
        m_audio_speed_adjust = clamp(error * c_speed_adjust_p_gain + m_speed_adjust_integral,
                                     -c_max_speed_adjust, c_max_speed_adjust);

        m_log.debug(eAudio, std::format("Audio buffer size: {}, adjustment: {:.5f} (integral {:.5f})",
                                        audio_buffer_size, (double)m_audio_speed_adjust, m_speed_adjust_integral));
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

// Real-time audio thread: no locking, allocation or logging in here.
void AudioPlayback::audioCallbackMember(void *output_buffer, unsigned long frames_per_buffer) {
    auto *out = (int16_t *)output_buffer;
    int read_ix = m_next_audio_buffer_read_ix;
    double speed_adjust = m_audio_speed_adjust;

    for (unsigned long i = 0; i < frames_per_buffer; i++) {
        int write_ix = m_next_audio_buffer_write_ix;
        int available = write_ix >= read_ix ? write_ix - read_ix :
                        c_audio_buffer_size - read_ix + write_ix;

        if (m_prebuffering) {
            if (available < c_audio_buffer_optimal_filled) {
                for (int k = 0; k < m_channels_used; k++)
                    *out++ = 0;
                continue;
            }
            // Consume one frame as interpolation history; the producer never writes the
            // slot just behind the read index, so it stays stable once we move past it.
            read_ix = read_ix + 1 == c_audio_buffer_size ? 0 : read_ix + 1;
            available--;
            m_read_frac = 0;
            m_prebuffering = false;
        }

        // Cubic interpolation over the frames at read_ix - 1 .. read_ix + 2, with the
        // fractional position between read_ix and read_ix + 1; available >= 3 is
        // guaranteed by the prebuffering gate and the advance loop below.
        int prev_ix = read_ix == 0 ? c_audio_buffer_size - 1 : read_ix - 1;
        int next_ix = read_ix + 1 == c_audio_buffer_size ? 0 : read_ix + 1;
        int next2_ix = next_ix + 1 == c_audio_buffer_size ? 0 : next_ix + 1;
        for (int k = 0; k < m_channels_used; k++) { // in c_channel_map order
            auto value = cubicInterpolate(m_audio_buffer[prev_ix].samples[k], m_audio_buffer[read_ix].samples[k],
                                          m_audio_buffer[next_ix].samples[k], m_audio_buffer[next2_ix].samples[k],
                                          (float)m_read_frac);
            *out++ = (int16_t)clamp(lroundf(value), -32768l, 32767l); // cubic can overshoot the sample range
        }

        m_read_frac += 1.0 + speed_adjust;
        while (m_read_frac >= 1.0) {
            if (available == 3) { // keep three frames ahead so the interpolation window stays valid
                m_underrun_count.fetch_add(1, std::memory_order_relaxed);
                m_prebuffering = true;
                m_read_frac = 0;
                break;
            }
            read_ix = next_ix;
            next_ix = next2_ix;
            next2_ix = next2_ix + 1 == c_audio_buffer_size ? 0 : next2_ix + 1;
            available--;
            m_read_frac -= 1.0;
        }
        m_next_audio_buffer_read_ix = read_ix;
    }
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
    config.resampling.linear.lpfOrder = MA_MAX_FILTER_ORDER; // best quality for the conversion to the device rate
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
