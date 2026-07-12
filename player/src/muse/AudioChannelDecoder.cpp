// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#include <algorithm>
#include "AudioChannelDecoder.h"

int AudioChannelDecoder::binaryBigEndianSeqToIntSigned(const int *s, int l) {
    int sum = 0;
    for (int i = 0; i < l; i++)
        sum += s[i] * (i == 0 ? -(1 << (l - 1)) : 1 << (l - i - 1)) ;
    return sum;
}

int AudioChannelDecoder::binaryLittleEndianSeqToIntUnsigned(const int *s, int l) {
    int sum = 0;
    for (int i = 0; i < l; i++)
        sum += s[i] * (1 <<  i);
    return sum;
}

const int AModeChannelDecoder::c_max_value = 16383;
const int AModeChannelDecoder::c_min_value = -16384;

const int BModeChannelDecoder::c_max_value = 32767;
const int BModeChannelDecoder::c_min_value = -32678;

int16_t AModeChannelDecoder::decodeSample(int range[3], bool range_ok, int data[8], bool data_ok) {
    if (range_ok) { // otherwise just repeat the previous sample; probably not good
        int multiplier = 128 >> binaryLittleEndianSeqToIntUnsigned(range, 3); // 1 to 128
        int dataValue = binaryBigEndianSeqToIntSigned(data, 8);
        m_value = m_value - m_value / 16 + dataValue * multiplier;
    }
    m_value = std::clamp(m_value, c_min_value, c_max_value);
    return (short)(m_value * 2);
}

int16_t BModeChannelDecoder::decodeSample(int range[3], bool range_ok, int data[11], bool data_ok) {
    if (range_ok) { // otherwise just repeat the previous sample; probably not good
        int multiplier = 32 >> binaryLittleEndianSeqToIntUnsigned(range, 3); // 1 to 32
        int dataValue = binaryBigEndianSeqToIntSigned(data, 11);
        m_value = m_value - m_value / 16 + dataValue * multiplier;
    }
    m_value = std::clamp(m_value, c_min_value, c_max_value);
    return (short)m_value;
}
