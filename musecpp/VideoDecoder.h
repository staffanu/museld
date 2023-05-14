//
// Created by staffanu on 4/9/23.
//

#ifndef MUSECPP_VIDEODECODER_H
#define MUSECPP_VIDEODECODER_H

#include <opencv2/core/mat.hpp>
#include "FieldBufferView.h"
#include "Shaders.h"
#include "MuseBuffer.h"

class VideoDecoder {
public:
    VideoDecoder();

    cv::Mat DecodeSingleField(FieldBufferView &field);

private:
    template<typename T> static MuseBuffer<T> CreateMuseBuffer(unsigned int height, unsigned int width);

    MuseBuffer<float> m_interpolated32_buffer;
    MuseBuffer<float> m_field_Y_buffer;
    MuseBuffer<float> m_field_C_r_buffer;
    MuseBuffer<float> m_field_C_b_buffer;
    MuseBuffer<float> m_intermediate_r_buffer;
    MuseBuffer<float> m_intermediate_b_buffer;
    MuseBuffer<uint32_t> m_frame_out_buffer;
};

template<typename T>
MuseBuffer<T> VideoDecoder::CreateMuseBuffer(unsigned int height, unsigned int width) {
    kp::Tensor::TensorDataTypes dataType;
    if (std::is_same<T, float>::value)
        dataType = kp::Tensor::TensorDataTypes::eFloat;
    else if (std::is_same<T, int32_t>::value)
        dataType = kp::Tensor::TensorDataTypes::eInt;
    else if (std::is_same<T, uint32_t>::value)
        dataType = kp::Tensor::TensorDataTypes::eUnsignedInt;
    else
        throw std::runtime_error("Unknown type T");

    // the initial data isn't used
    auto *data = new T[height * width];
    auto tensor = Shaders::GetManager().tensor(data, height * width, sizeof(T), dataType, kp::Tensor::TensorTypes::eDevice);
    auto buffer = MuseBuffer<T>(height, width, tensor);
    delete[] data;
    return buffer;
}

#endif //MUSECPP_VIDEODECODER_H
