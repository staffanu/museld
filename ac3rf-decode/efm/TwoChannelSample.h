//
// Created by Staffan Ulfberg on 11/4/25.
//

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