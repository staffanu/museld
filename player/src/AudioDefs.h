// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_AUDIODEFS_H
#define MUSECPP_AUDIODEFS_H

#include <cstdint>

#define MAX_AUDIO_OUTPUT_SAMPLES 2048

enum AudioMode { MODE_A, MODE_B, MODE_EFM, MODE_UNKNOWN };

struct AudioFrame
{
    int16_t samples[4];
};

#endif //MUSECPP_AUDIODEFS_H
