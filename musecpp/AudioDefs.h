//
// Created by staffanu on 2/8/24.
//

#ifndef MUSECPP_AUDIODEFS_H
#define MUSECPP_AUDIODEFS_H

#define MAX_AUDIO_OUTPUT_SAMPLES 2048

enum AudioMode { MODE_A, MODE_B, MODE_EFM, MODE_UNKNOWN };

struct AudioFrame
{
    int16_t samples[4];
};

#endif //MUSECPP_AUDIODEFS_H
