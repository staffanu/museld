// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cstdint>
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
const int BModeChannelDecoder::c_min_value = -32768;

// The DPCM of the NHK book: the value leaks by a sixteenth per sample and
// the transmitted word, scaled by the block's range, is added.  When the
// word is unusable the leak alone is applied (a zero delta, the encoder's
// own prediction), the sample is flagged as erased for the concealer, and
// the error it leaves in the value decays by the same leak within a few
// dozen samples.  Before, an uncorrectable data word was added as it came
// (the row's BCH verdict was ignored), and a bad range word froze the value.
int16_t AModeChannelDecoder::decodeSample(const int range[3], bool range_ok, const int data[8], bool data_ok, bool *erased) {
    const bool ok = range_ok && data_ok;
    const int multiplier = 128 >> binaryLittleEndianSeqToIntUnsigned(range, 3); // 1 to 128
    const int delta = ok ? binaryBigEndianSeqToIntSigned(data, 8) * multiplier : 0;
    m_value = std::clamp(m_value - m_value / 16 + delta, c_min_value, c_max_value);
    *erased = !ok;
    return (int16_t)(m_value * 2);
}

int16_t BModeChannelDecoder::decodeSample(const int range[3], bool range_ok, const int data[11], bool data_ok, bool *erased) {
    const bool ok = range_ok && data_ok;
    const int multiplier = 32 >> binaryLittleEndianSeqToIntUnsigned(range, 3); // 1 to 32
    const int delta = ok ? binaryBigEndianSeqToIntSigned(data, 11) * multiplier : 0;
    m_value = std::clamp(m_value - m_value / 16 + delta, c_min_value, c_max_value);
    *erased = !ok;
    return (int16_t)m_value;
}
