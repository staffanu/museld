//
// Created by staffanu on 4/9/23.
//

#include <iostream>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include "VideoDecoder.h"
#include "FilterUtil.h"

using namespace cv;
using namespace std;

VideoDecoder::VideoDecoder() = default;

void print_stats(const string &header, uint8_t const *data, size_t length) {
    uint8_t mx = 0;
    uint8_t mn = 255;
    long sum = 0;
    for (size_t i = 0; i < length; i++) {
        sum += data[i];
        mx = max(mx, data[i]);
        mn = min(mn, data[i]);
    }
    cout << header << ": min: " << (int)mn << ", max: " << (int)mx <<
        ", avg: " << (sum / length) << endl;
}

void show_buffer(int x_min, int width, int y_min, int height,
                 uint8_t (&buffer_r)[MUSE_BUF_HEIGHT][MUSE_Y_BUF_WIDTH],
                 uint8_t (&buffer_b)[MUSE_BUF_HEIGHT][MUSE_Y_BUF_WIDTH]) {
    Mat mat(height, width, CV_8UC3);
    for (int row = y_min; row < y_min + height; row++) {
        auto *mat_row = mat.ptr<Vec3b>(row - y_min);
        for (int col = x_min; col < x_min + width; col++) {
            int RmYM = buffer_r[row][col] - 128;
            int BmYM = buffer_b[row][col] - 128;
            int YM = 128;
            int g = clamp(YM - BmYM / 4 - RmYM / 2, 0, 255);
            int b = clamp(YM + BmYM + BmYM / 4, 0, 255);
            int r = clamp(YM + RmYM, 0, 255);
            mat_row[col - x_min] = Vec3b(b, g, r);
        }
    }
    namedWindow("MUSE", WINDOW_NORMAL);
    resizeWindow("MUSE", 16 * 100, 9 * 100);
    imshow("MUSE", mat);
    waitKey(0);
}


void VideoDecoder::DecodeSingleField(FieldBufferView &field, Mat &field_out) {
    uint8_t field_Y[MUSE_BUF_HEIGHT * 2][MUSE_Y_BUF_WIDTH * 3];
    DecodeSingleFieldY(field, field_Y);

    uint8_t field_C_r[MUSE_BUF_HEIGHT][MUSE_Y_BUF_WIDTH];
    uint8_t field_C_b[MUSE_BUF_HEIGHT][MUSE_Y_BUF_WIDTH];
    DecodeSingleFieldC(field, field_C_r, field_C_b);

    CombineYandRB(field_Y, field_C_r, field_C_b, field_out);
//    for (int row = 0; row < MUSE_BUF_HEIGHT * 2; row++) {
//        for (int col = 0; col < MUSE_Y_BUF_WIDTH * 3; col++) {
//            field_out.data[row * MUSE_Y_BUF_WIDTH * 3 + col] = field_Y[row][col];
//        }
//    }
}

void VideoDecoder::DecodeSingleFieldY(FieldBufferView &field,
                                      uint8_t (&out)[MUSE_BUF_HEIGHT * 2][MUSE_Y_BUF_WIDTH * 3]) {
    int field_parity = field.m_field_parity;
    int inter_frame_phase = field.m_control.has_value() ?
            field.m_control.value().frame_subsampling_phase_Y.value_or(0) : 0;
    // int inter_field_phase = field.m_control.value().field_subsampling_phase_Y.value_or(0);
    auto frame_buffer_Y = field.frame_buffer_Y();

    cout << "Decoding frame " << field.m_frame_no << " field " << field.m_field_parity << endl;
    uint8_t interpolated32[MUSE_BUF_HEIGHT][MUSE_Y_BUF_WIDTH * 2];

    for (int row = 0; row < MUSE_BUF_HEIGHT; row++) {
        int phase = (inter_frame_phase + row + 1) % 2; // notice row is even when line no is odd (row 0 is really line 47/609)
        for (int col = 0; col < MUSE_Y_BUF_WIDTH; col++) {
            interpolated32[row][col * 2 + phase] = frame_buffer_Y(row, col);
            // interpolated32[row][col * 2 + 1 - phase] = 0; // not needed -- written when filtering
        }
    }

    FilterUtil::FilterImageDiamond(inter_frame_phase, interpolated32);

    // convert sample rate from 32.4 MHz to 48.6 MHz
    for (int row = 0; row < MUSE_BUF_HEIGHT; row++)
        FilterUtil::ConvertSampleRate2to3(MUSE_Y_BUF_WIDTH * 2, interpolated32[row],
                                          out[2 * row + 1 - field_parity]);

    // probably not the best interpolation of missing lines but looks more or less ok
    for (int row = 0; row < MUSE_BUF_HEIGHT; row++) {
        int empty_row = 2 * row + field_parity;
        for (int col = 0; col < MUSE_Y_BUF_WIDTH * 3; col++) {
            int row_above = empty_row == 0 ? 1 : empty_row - 1;
            int row_below = empty_row == MUSE_BUF_HEIGHT * 2 - 1 ? MUSE_BUF_HEIGHT * 2 - 2 : empty_row + 1;
            out[empty_row][col] = (out[row_above][col] + out[row_below][col]) / 2;
        }
    }
}

void VideoDecoder::DecodeSingleFieldC(FieldBufferView &field,
                                      uint8_t (&out_r)[MUSE_BUF_HEIGHT][MUSE_Y_BUF_WIDTH],
                                      uint8_t (&out_b)[MUSE_BUF_HEIGHT][MUSE_Y_BUF_WIDTH]) {
    auto frame_buffer_C = field.frame_buffer_C();
    int frame_phase = field.m_control.has_value() ?
            field.m_control.value().frame_subsampling_phase_C.value_or(0) : 0;
    int field_parity = field.m_field_parity;

    uint8_t intermediate_r[MUSE_BUF_HEIGHT][MUSE_Y_BUF_WIDTH];
    uint8_t intermediate_b[MUSE_BUF_HEIGHT][MUSE_Y_BUF_WIDTH];
    memset(intermediate_r, 128, sizeof(intermediate_r));
    memset(intermediate_b, 128, sizeof(intermediate_b));

    for (int row = 0; row < MUSE_BUF_HEIGHT; row++) {
        int line = row + 43 + field_parity * 562; // line no as defined in MUSE; we could remove the second term since it is always even
        int offset = (frame_phase + line + 1) % 2 * 2 + field_parity;

        auto output = line % 2 == 1 ? intermediate_r : intermediate_b;
        for (int col = 0; col < MUSE_C_BUF_WIDTH; col++)
            if (col * 4 + offset < MUSE_Y_BUF_WIDTH)
                output[row][col * 4 + offset] = frame_buffer_C(row, col);
    }

    //int size = 1;
    //show_buffer(0, MUSE_Y_BUF_WIDTH / size, 0, MUSE_BUF_HEIGHT / size, intermediate_r, intermediate_b);

    FilterUtil::FilterImage(FilterUtil::s_color_filter, intermediate_r, out_r, 128, 8);
    FilterUtil::FilterImage(FilterUtil::s_color_filter, intermediate_b, out_b, 128, 8);

    //show_buffer(0, MUSE_Y_BUF_WIDTH / size, 0, MUSE_BUF_HEIGHT / size, out_r, out_b);
}

void VideoDecoder::CombineYandRB(
        const uint8_t (&field_Y)[MUSE_BUF_HEIGHT * 2][MUSE_Y_BUF_WIDTH * 3],
        const uint8_t (&field_C_r)[MUSE_BUF_HEIGHT][MUSE_Y_BUF_WIDTH],
        const uint8_t (&field_C_b)[MUSE_BUF_HEIGHT][MUSE_Y_BUF_WIDTH],
        Mat &out_rgb) {

    for (int row = 0; row < MUSE_BUF_HEIGHT * 2; row++) {
        auto *mat_row = out_rgb.ptr<Vec3b>(row);
        for (int col = 0; col < MUSE_Y_BUF_WIDTH * 3; col++) {
            int YM = field_Y[row][col];
            int RmYM = field_C_r[row / 2][col / 3] - 128;
            int BmYM = field_C_b[row / 2][col / 3] - 128;

            int g = clamp(YM - BmYM / 4 - RmYM / 2, 0, 255);
            int b = clamp(YM + BmYM + BmYM / 4, 0, 255);
            int r = clamp(YM + RmYM, 0, 255);

            mat_row[col] = Vec3b(b, g, r);
        }
    }
}
