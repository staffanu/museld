// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_COMPRESSEDAUDIODECODER_H
#define MUSECPP_COMPRESSEDAUDIODECODER_H

#include <cstdint>
#include <memory>
#include <vector>
#include "AudioDefs.h"

class Logger;

// Decodes an AC3 or DTS bitstream to multichannel PCM using libavcodec.  The
// input is a byte stream: whole AC3 sync frames from the AC3-RF path, or the
// raw sample bytes of a DTS laserdisc's EFM track; libavcodec's parser finds
// the frame boundaries either way, so partial or damaged frames only cost
// their own output.
//
// Surround streams come out in the AudioFrame 5.1 slot order (FL FR FC LFE
// SL SR, back channels mapped to the side slots); mono/stereo streams fill
// the first two slots and leave the rest silent.
//
// Without HAVE_LIBAV the class exists but decode() returns nothing (after
// warning once), which leaves the AC3/DTS tracks silent in builds without
// FFmpeg rather than making them a build-time error.
class CompressedAudioDecoder {
public:
    enum class Codec { eAc3, eDts };

    CompressedAudioDecoder(Logger &log, Codec codec);
    ~CompressedAudioDecoder();
    CompressedAudioDecoder(const CompressedAudioDecoder &) = delete;
    CompressedAudioDecoder &operator=(const CompressedAudioDecoder &) = delete;

    // True when this build can actually decode (compiled with HAVE_LIBAV).
    static bool available();

    // Feed bitstream bytes; returns whatever PCM became decodable.  A frame
    // that fails to decode is replaced by silence of the same duration, so
    // dropouts pause the sound instead of shifting the A/V sync.
    std::vector<AudioFrame> decode(const uint8_t *data, size_t size);

private:
    struct Impl;
    Logger &m_log;
    const Codec m_codec;
    bool m_unavailable_warned;
    std::unique_ptr<Impl> m_impl; // null when libav is unavailable or init failed
};

#endif //MUSECPP_COMPRESSEDAUDIODECODER_H
