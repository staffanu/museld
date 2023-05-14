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
: m_interpolated32_buffer(VideoDecoder::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH * 2)),
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
    cout << "Decoding frame " << field.m_frame_no << " field " << field.m_field_parity << endl;

    int field_parity = field.m_field_parity;
    int frame_phase_y = field.m_control.has_value() ?
            field.m_control.value().frame_subsampling_phase_Y.value_or(0) : 0;
    int frame_phase_c = field.m_control.has_value() ?
            field.m_control.value().frame_subsampling_phase_C.value_or(0) : 0;
    // int field_phase = field.m_control.value().field_subsampling_phase_Y.value_or(0);

    Shaders &shaders = Shaders::GetInstance();

    shaders.CopyYForInterpolation(*field.m_data, m_interpolated32_buffer, field_parity, frame_phase_y);
    shaders.FilterImageDiamond(frame_phase_y, m_interpolated32_buffer);
    shaders.ConvertHorizSampleRate2to3(m_interpolated32_buffer, m_field_Y_buffer, 1 - field_parity, 2);
    shaders.FillEmptyLines(m_field_Y_buffer, field_parity);

    shaders.DecodeC(*field.m_data, m_intermediate_r_buffer, m_intermediate_b_buffer,
                    frame_phase_c, field_parity);
    shaders.FilterImage(
            Shaders::s_color_filter_height, Shaders::s_color_filter_width,
            shaders.m_color_filter_tensor, m_intermediate_r_buffer, m_field_C_r_buffer, 128.0 / 8, 8);
    shaders.FilterImage(
            Shaders::s_color_filter_height, Shaders::s_color_filter_width,
            shaders.m_color_filter_tensor, m_intermediate_b_buffer, m_field_C_b_buffer, 128.0 / 8, 8);

    shaders.CombineYandRB(m_field_Y_buffer, m_field_C_r_buffer, m_field_C_b_buffer, m_frame_out_buffer);

    return { vector<int>{ MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3}, CV_8UC4, m_frame_out_buffer.data() };
}
