// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_AUDIOCHANNELDECODER_H
#define MUSECPP_AUDIOCHANNELDECODER_H

#include <cstdint>
#include <ios>
#include <fstream>

class AudioChannelDecoder {
protected:
    explicit AudioChannelDecoder(int channel) : m_channel(channel) {};
    static int binaryBigEndianSeqToIntSigned(const int *s, int l);
    static int binaryLittleEndianSeqToIntUnsigned(const int *s, int l);

protected:
    int m_channel;
};

class AModeChannelDecoder : public AudioChannelDecoder {
public:
    explicit AModeChannelDecoder(int channel) : AudioChannelDecoder(channel) {};
    // The sample, and whether it is a prediction only (*erased) because the
    // range word or the row's data word could not be corrected
    int16_t decodeSample(const int range[3], bool range_ok, const int data[8], bool data_ok, bool *erased);

private:
    static const int c_max_value;
    static const int c_min_value;
    int m_value = 0;
};

class BModeChannelDecoder : public AudioChannelDecoder {
public:
    explicit BModeChannelDecoder(int channel) : AudioChannelDecoder(channel) {};

    int16_t decodeSample(const int range[3], bool range_ok, const int data[11], bool data_ok, bool *erased);

private:
    static const int c_max_value;
    static const int c_min_value;
    int m_value = 0;
};

#endif //MUSECPP_AUDIOCHANNELDECODER_H
