//
// Created by Staffan Ulfberg on 11/4/25.
//

#ifndef AC3RF_EFM_DECODE_TWOCHANNELSAMPLE_H
#define AC3RF_EFM_DECODE_TWOCHANNELSAMPLE_H

#include <cstdint>

struct TwoChannelSample {
    int16_t left;
    int16_t right;
};

#endif //AC3RF_EFM_DECODE_TWOCHANNELSAMPLE_H