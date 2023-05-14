//
// Created by staffanu on 4/9/23.
//

#include <iostream>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include "VideoDecoder.h"
#include "MuseBuffer.h"

using namespace cv;
using namespace std;

VideoDecoder::VideoDecoder()
: m_frame_in_buffer(VideoDecoder::CreateMuseBuffer<float>(MUSE_TOTAL_HEIGHT, MUSE_TOTAL_WIDTH)),
  m_interpolated32_buffer(VideoDecoder::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH * 2)),
  m_field_Y_buffer(VideoDecoder::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3)),
  m_field_C_r_buffer(VideoDecoder::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_field_C_b_buffer(VideoDecoder::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_intermediate_r_buffer(VideoDecoder::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_intermediate_b_buffer(VideoDecoder::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_frame_out_buffer(VideoDecoder::CreateMuseBuffer<uint32_t>(MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3))
{
}

void show_buffer(int y_min, int x_min, int height, int width, const MuseBuffer<float> &buffer) {
    Mat mat(height, width, CV_32FC1);
    for (int row = y_min; row < y_min + height; row++) {
        auto *mat_row = mat.ptr<float>(row - y_min);
        for (int col = x_min; col < x_min + width; col++) {
            mat_row[col - x_min] = buffer[row][col] / 255.0f;
        }
    }
    namedWindow("MUSE", WINDOW_NORMAL);
    resizeWindow("MUSE", 16 * 100, 9 * 100);
    imshow("MUSE", mat);
    waitKey(0);
}

Mat VideoDecoder::DecodeSingleField(FieldBufferView &field) {
    DecodeSingleFieldY(field, m_field_Y_buffer);
    DecodeSingleFieldC(field, m_field_C_r_buffer, m_field_C_b_buffer);
    Shaders::GetInstance().CombineYandRB(m_field_Y_buffer, m_field_C_r_buffer, m_field_C_b_buffer, m_frame_out_buffer);

    return { vector<int>{ MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3}, CV_8UC4, m_frame_out_buffer.data() };
}

void VideoDecoder::DecodeSingleFieldY(FieldBufferView &field, MuseBuffer<float> &out) {
    int field_parity = field.m_field_parity;
    int inter_frame_phase = field.m_control.has_value() ?
            field.m_control.value().frame_subsampling_phase_Y.value_or(0) : 0;
    // int inter_field_phase = field.m_control.value().field_subsampling_phase_Y.value_or(0);
    auto frame_buffer_Y = field.frame_buffer_Y();

    cout << "Decoding frame " << field.m_frame_no << " field " << field.m_field_parity << endl;

    for (int row = 0; row < MUSE_BUF_HEIGHT; row++) {
        int phase = (inter_frame_phase + row + 1) % 2; // notice row is even when line no is odd (row 0 is really line 47/609)
        for (int col = 0; col < MUSE_Y_BUF_WIDTH; col++) {
            m_interpolated32_buffer[row][col * 2 + phase] = frame_buffer_Y[row][col];
            // interpolated32[row][col * 2 + 1 - phase] = 0; // not needed -- written when filtering
        }
    }

    Shaders::GetInstance().FilterImageDiamond(inter_frame_phase, m_interpolated32_buffer);
    Shaders::GetInstance().ConvertHorizSampleRate2to3(m_interpolated32_buffer, out, 1 - field_parity, 2);
    Shaders::GetInstance().FillEmptyLines(out, field_parity);
}

void VideoDecoder::DecodeSingleFieldC(FieldBufferView &field, MuseBuffer<float> &out_r, MuseBuffer<float> &out_b) {
    auto frame_buffer_C = field.frame_buffer_C();
    int frame_phase = field.m_control.has_value() ?
            field.m_control.value().frame_subsampling_phase_C.value_or(0) : 0;
    int field_parity = field.m_field_parity;

    Shaders &shaders = Shaders::GetInstance();

    for (int i = 0; i < MUSE_TOTAL_HEIGHT; i++)
        for (int j = 0; j < MUSE_TOTAL_WIDTH; j++)
            m_frame_in_buffer[i][j] = (*field.m_data)[i][j];

    shaders.DecodeC(m_frame_in_buffer, m_intermediate_r_buffer, m_intermediate_b_buffer,
                    frame_phase, field_parity);

    shaders.FilterImage(
            Shaders::s_color_filter_height, Shaders::s_color_filter_width,
            shaders.m_color_filter_tensor, m_intermediate_r_buffer, out_r, 128.0 / 8, 8);
    shaders.FilterImage(
            Shaders::s_color_filter_height, Shaders::s_color_filter_width,
            shaders.m_color_filter_tensor, m_intermediate_b_buffer, out_b, 128.0 / 8, 8);
}
;