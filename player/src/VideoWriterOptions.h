//
// Created by staffanu on 7/3/26.
//

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
