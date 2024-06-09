//
// Created by staffanu on 4/9/23.
//

#ifndef MUSECPP_AUDIODECODER_H
#define MUSECPP_AUDIODECODER_H

#include <array>
#include <deque>
#include <map>
#include <string>
#include <vector>
#include "musevk/VulkanBuffer.h"
#include "BchDecoder.h"
#include "AudioChannelDecoder.h"
#include "AudioDefs.h"

class Logger;

class AudioDecoder {
public:
    explicit AudioDecoder(Logger &log);
    AudioDecoder(const AudioDecoder &) = delete;
    void operator=(const AudioDecoder &) = delete;


    // output samples are written to the first two or all four channels depending on the detected mode
    void decodeFrame(int frame_no,
                     std::shared_ptr<musevk::VulkanBuffer> const &audio_converted_freq,
                     AudioMode *audio_mode,
                     int *sample_count,
                     AudioFrame output_samples[MAX_AUDIO_OUTPUT_SAMPLES]);

private:
    static std::vector<std::pair<float, float>> defaultSymbolLocations();
    static AudioMode detectModeFromControlData(uint32_t control_data);

    static const std::vector<std::pair<float, float>> c_default_symbol_locations;
    static const int c_deinterleave_stages = 25;
    static const int c_deinterleave_buffer_size =  (c_deinterleave_stages - 1) * 1350 + 1;
    static const std::array<std::array<bool, 3>, 8> c_symbol_bits;
    static const std::array<bool, 16> c_sync_pattern;

    Logger &m_log;
    bool m_deinterleave_data[c_deinterleave_buffer_size];
    int m_deinterleave_buffer_start; // start of circular buffer
    int m_total_deinterleaved_bits;
    int m_q;

    int m_consecutive_failed_syncs;

    std::vector<std::pair<float, float>> m_symbol_locations;
    std::deque<bool> m_queue;

    std::deque<uint32_t> m_control_signals; // 22 bits used
    uint32_t m_active_control_signal; // 22 bits used
    AudioMode m_active_audio_mode;

    BchDecoder m_bch_decoder;
    BchDecoder m_range_bch_decoder;

    AModeChannelDecoder aModeChannel1Decoder;
    AModeChannelDecoder aModeChannel2Decoder;
    AModeChannelDecoder aModeChannel3Decoder;
    AModeChannelDecoder aModeChannel4Decoder;
    BModeChannelDecoder bModeChannel1Decoder;
    BModeChannelDecoder bModeChannel2Decoder;

    long m_total_time_us;
};


#endif //MUSECPP_AUDIODECODER_H
