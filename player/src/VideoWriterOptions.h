// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_VIDEOWRITEROPTIONS_H
#define MUSECPP_VIDEOWRITEROPTIONS_H

// Kept free of ffmpeg dependencies so museld.cpp can parse command line
// options even in builds without libav.

#include <string>
#include "AudioDefs.h"

enum class VideoWriterPreset {
    eStandard, // H.264 (libx264, CRF) + AAC in MP4 -- compatible with everything
    eArchival, // FFV1 (lossless, 16 bpp) + PCM in Matroska
};

enum class VideoColorStandard {
    eBt709,     // HD (MUSE)
    eSmpte170m, // SD (NTSC)
    eBt470bg,   // SD (PAL)
};

// One audio stream of a --write file
struct WriterAudioStream {
    AudioTrack track;     // the decoded track it takes
    bool ac3_passthrough; // the stream is the original AC3 bitstream (stream copy); else PCM
    std::string title;    // the stream's title tag
};

#endif //MUSECPP_VIDEOWRITEROPTIONS_H
