//
// Created by staffanu on 4/9/23.
//

#ifndef MUSECPP_AUDIODECODER_H
#define MUSECPP_AUDIODECODER_H

#include <array>
#include <deque>
#include <map>
#include <string>
#include "BchDecoder.h"
#include "AudioChannelDecoder.h"
#include "Logger.h"

class MuseBuffer;

class AudioDecoder {
public:
    static const size_t c_max_output_samples = 2048;
    enum AudioMode { MODE_A, MODE_B, MODE_UNKNOWN };

    struct AudioFrame
    {
        int16_t samples[4];
    };

    AudioDecoder(Logger &log);

    // output samples are written to the first two or all four channels depending on the detected mode
    void decodeFrame(int frame_no,
                     MuseBuffer &audio_converted_freq,
                     AudioMode &audio_mode,
                     size_t &sample_count,
                     AudioFrame output_samples[c_max_output_samples]);

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
};


#endif //MUSECPP_AUDIODECODER_H
