//
// Created by staffanu on 4/9/23.
//

#ifndef MUSECPP_VIDEODECODER_H
#define MUSECPP_VIDEODECODER_H

#include <opencv2/core/mat.hpp>
#include "FieldBufferView.h"

class VideoDecoder {
public:
    VideoDecoder();
    void DecodeSingleField(FieldBufferView &field, cv::Mat &field_out);
};

#endif //MUSECPP_VIDEODECODER_H
