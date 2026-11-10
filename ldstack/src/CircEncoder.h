// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef LDSTACK_CIRCENCODER_H
#define LDSTACK_CIRCENCODER_H

#include <array>
#include <cstdint>
#include <vector>

// CIRC encoder (IEC 60908): the exact mirror of museld's EfmDecoder, so that
// the 32 data/parity symbols of every EFM frame can be predicted from the
// audio.  Frame f (0-based) is built from stereo samples 6 f + phase .. + 5;
// the output is what the disc carries (the decoder's "F:" dump), parity
// inverted, all delay lines included, so predicted frame g corresponds to a
// received frame with the same index up to a constant offset.
struct EncodedFrames {
    int first_valid = 0;                 // frames before this are missing delay-line history
    std::vector<std::array<uint8_t, 32>> frames;
};

// samples: interleaved L, R 16-bit; count is the number of stereo samples
EncodedFrames circEncode(const int16_t *samples, int64_t count, int phase);

#endif //LDSTACK_CIRCENCODER_H
