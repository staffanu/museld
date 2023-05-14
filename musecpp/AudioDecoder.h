//
// Created by staffanu on 4/9/23.
//

#ifndef MUSECPP_AUDIODECODER_H
#define MUSECPP_AUDIODECODER_H


#include "FieldBufferView.h"

class AudioDecoder {
public:
    AudioDecoder();
    void decode_field(MuseSubBuffer const &data);
};


#endif //MUSECPP_AUDIODECODER_H
