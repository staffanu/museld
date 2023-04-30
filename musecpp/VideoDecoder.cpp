//
// Created by staffanu on 4/9/23.
//

#include <iostream>
#include "VideoDecoder.h"
#include "FilterUtil.h"

using namespace cv;
using namespace std;

VideoDecoder::VideoDecoder() {
}

void print_max_and_avg(string header, DecoderInt const *data, size_t length) {
    DecoderInt m = 0;
    long sum = 0;
    for (size_t i = 0; i < length; i++) {
        sum += data[i];
        m = max(m, data[i]);
    }
    cout << header << ": max: " << (int)m << ", avg: " << (sum / length) << endl;
}

void VideoDecoder::DecodeSingleField(FieldBufferView &field, Mat &field_out) {
    int field_parity = field.m_field_parity;
    int inter_frame_phase = field.m_control.has_value() ?
            field.m_control.value().frame_subsampling_phase_Y.value_or(0) : 0;
    // int inter_field_phase = field.m_control.value().field_subsampling_phase_Y.value_or(0);
    auto frame_buffer_Y = field.frame_buffer_Y();

    cout << "Decoding frame " << field.m_frame_no << " field " << field.m_field_parity << endl;
    DecoderInt interpolated32[MUSE_Y_BUF_HEIGHT][MUSE_Y_BUF_WIDTH * 2];

    for (int row = 0; row < MUSE_Y_BUF_HEIGHT; row++) {
        int phase = (inter_frame_phase + row + 1) % 2; // notice row is even when line no is odd (row 0 is really line 47/609)
        for (int col = 0; col < MUSE_Y_BUF_WIDTH; col++) {
            interpolated32[row][col * 2 + phase] = frame_buffer_Y(row, col);
            // interpolated32[row][col * 2 + 1 - phase] = 0;
        }
    }

    FilterUtil::FilterImageDiamond(inter_frame_phase, interpolated32);

    print_max_and_avg("interpolated32", interpolated32[0], MUSE_Y_BUF_HEIGHT * MUSE_Y_BUF_WIDTH * 2);

    // convert sample rate from 32.4 MHz to 48.6 MHz
    for (int row = 0; row < MUSE_Y_BUF_HEIGHT; row++)
        FilterUtil::ConvertSampleRate2to3(MUSE_Y_BUF_WIDTH * 2, interpolated32[row],
                                          &field_out.data[(2 * row + 1 - field_parity) * MUSE_Y_BUF_WIDTH * 3]);

    // print_max_and_avg("48 MHz", field_out.data, MUSE_Y_BUF_HEIGHT * 2 * MUSE_Y_BUF_WIDTH * 3);

    // probably not the best interpolation of missing lines but looks more or less ok
    for (int row = 0; row < MUSE_Y_BUF_HEIGHT; row++) {
        int empty_row = 2 * row + field_parity;
        for (int col = 0; col < MUSE_Y_BUF_WIDTH * 3; col++) {
            int row_above = max(empty_row - 1, 0);
            int row_below = min(empty_row + 1, MUSE_Y_BUF_HEIGHT * 2 - 1);
            field_out.data[empty_row * MUSE_Y_BUF_WIDTH * 3 + col] =
                    (field_out.data[row_above * MUSE_Y_BUF_WIDTH * 3 + col] +
                    field_out.data[row_below * MUSE_Y_BUF_WIDTH * 3 + col]) / 2;
        }
    }

    // print_max_and_avg("Output", field_out.data, MUSE_Y_BUF_HEIGHT * 2 * MUSE_Y_BUF_WIDTH * 3);
}
