// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_AUDIODEFS_H
#define MUSECPP_AUDIODEFS_H

#include <cstdint>

#define MAX_AUDIO_OUTPUT_SAMPLES 2048
#define MAX_AUDIO_CHANNELS 6

enum AudioMode { MODE_A, MODE_B, MODE_EFM, MODE_ANALOG, MODE_AC3, MODE_DTS, MODE_UNKNOWN };

// Which audio source the user selected (the A key / --efm / --ac3).  eDefault
// is the MUSE audio on MUSE discs and the analog FM audio on NTSC discs;
// eAc3 (the AC3-RF QPSK carrier) exists on NTSC discs only.  DTS is not a
// track of its own: it arrives on the EFM track and is detected there.
enum class AudioTrack { eDefault, eEfm, eAc3 };

// One output sample across the channels of the playing mode.  The slot
// meaning depends on the mode (AudioPlayback carries the channel map):
//   MODE_A (MUSE 4-channel):  FL FR BL BR
//   MODE_AC3 / MODE_DTS 5.1:  FL FR FC LFE SL SR
//   everything else (stereo): FL FR
struct AudioFrame
{
    int16_t samples[MAX_AUDIO_CHANNELS];
};

#endif //MUSECPP_AUDIODEFS_H
