// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#ifndef MUSECPP_VIDEOWRITEROPTIONS_H
#define MUSECPP_VIDEOWRITEROPTIONS_H

// Kept free of ffmpeg dependencies so museld.cpp can parse command line
// options even in builds without libav.

enum class VideoWriterPreset {
    eStandard, // H.264 (libx264, CRF) + AAC in MP4 -- compatible with everything
    eArchival, // FFV1 (lossless, 16 bpp) + PCM in Matroska
};

enum class VideoColorStandard {
    eBt709,     // HD (MUSE)
    eSmpte170m, // SD (NTSC)
};

#endif //MUSECPP_VIDEOWRITEROPTIONS_H
