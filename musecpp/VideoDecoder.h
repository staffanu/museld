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
    static void DecodeSingleField(FieldBufferView &field, cv::Mat &field_out);

private:
    static void DecodeSingleFieldY(
            FieldBufferView &field,
            uint8_t (&out)[MUSE_BUF_HEIGHT * 2][MUSE_Y_BUF_WIDTH * 3]);
    static void DecodeSingleFieldC(
            FieldBufferView &field,
            uint8_t (&out_r)[MUSE_BUF_HEIGHT][MUSE_Y_BUF_WIDTH],
            uint8_t (&out_b)[MUSE_BUF_HEIGHT][MUSE_Y_BUF_WIDTH]);
    static void CombineYandRB(
            const uint8_t (&field_Y)[MUSE_BUF_HEIGHT * 2][MUSE_Y_BUF_WIDTH * 3],
            const uint8_t (&field_C_r)[MUSE_BUF_HEIGHT][MUSE_Y_BUF_WIDTH],
            const uint8_t (&field_C_b)[MUSE_BUF_HEIGHT][MUSE_Y_BUF_WIDTH],
            cv::Mat &out_rgb);
};

#endif //MUSECPP_VIDEODECODER_H
