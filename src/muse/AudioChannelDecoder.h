//
// Created by staffanu on 6/21/23.
//

#ifndef MUSECPP_AUDIOCHANNELDECODER_H
#define MUSECPP_AUDIOCHANNELDECODER_H

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
    int16_t decodeSample(int range[3], bool range_ok, int data[8], bool data_ok);

private:
    static const int c_max_value;
    static const int c_min_value;
    int m_value = 0;
};

class BModeChannelDecoder : public AudioChannelDecoder {
public:
    explicit BModeChannelDecoder(int channel) : AudioChannelDecoder(channel) {};

    int16_t decodeSample(int range[3], bool range_ok, int data[11], bool data_ok);

private:
    static const int c_max_value;
    static const int c_min_value;
    int m_value = 0;
};

#endif //MUSECPP_AUDIOCHANNELDECODER_H
