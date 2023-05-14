//
// Created by staffanu on 5/13/23.
//

#ifndef MUSECPP_MUSEBUFFER_H
#define MUSECPP_MUSEBUFFER_H

#include <cstddef>
#include <type_traits>
#include <kompute/Kompute.hpp>
#include <iostream>
#include "Shaders.h"

template<typename T>
class MuseBuffer {
public:
    MuseBuffer(unsigned int height, unsigned int width, T *data);

    unsigned int width() {
        return m_width;
    }

    unsigned int height() {
        return m_height;
    }

    std::shared_ptr<kp::Tensor> tensor() {
        return m_tensor;
    }

    size_t byte_size() {
        return m_height * m_width * sizeof(T);
    }

    T *data() {
        return m_tensor->data<T>();
    }

    const T *operator [](size_t row) const
    {
        return m_tensor->data<T>() + row * m_width;
    }

    T *operator [](size_t row)
    {
        return m_tensor->data<T>() + row * m_width;
    }

private:
    unsigned int m_height;
    unsigned int m_width;
    std::shared_ptr<kp::Tensor> m_tensor;
};

template<typename T>
MuseBuffer<T>::MuseBuffer(unsigned int height, unsigned int width, T *data) :
        m_height(height),
        m_width(width) {
    kp::Tensor::TensorDataTypes dataType;
    if (std::is_same<T, float>::value)
        dataType = kp::Tensor::TensorDataTypes::eFloat;
    else if (std::is_same<T, int32_t>::value)
        dataType = kp::Tensor::TensorDataTypes::eInt;
    else if (std::is_same<T, uint32_t>::value)
        dataType = kp::Tensor::TensorDataTypes::eUnsignedInt;
    else
        throw std::runtime_error("Unknown type T");

    m_tensor = Shaders::GetManager().tensor(data, height * width, sizeof(T), dataType, kp::Tensor::TensorTypes::eDevice);
}

#endif //MUSECPP_MUSEBUFFER_H
