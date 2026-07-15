// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_EFM_DECODE_TWOCHANNELSAMPLE_H
#define AC3RF_EFM_DECODE_TWOCHANNELSAMPLE_H

#include <cstdint>

struct TwoChannelSampleWithErasureFlags {
    bool erased[2];
    int16_t samples[2];
};

struct TwoChannelSample {
    int16_t samples[2];
};

#endif //AC3RF_EFM_DECODE_TWOCHANNELSAMPLE_H