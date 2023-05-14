//
// Created by staffanu on 5/13/23.
//

#ifndef MUSECPP_MUSEBUFFER_H
#define MUSECPP_MUSEBUFFER_H

#include <cstddef>
#include <type_traits>
#include <kompute/Kompute.hpp>
#include <iostream>

template<typename T>
class MuseBuffer {
public:
    MuseBuffer(unsigned int height, unsigned int width, std::shared_ptr<kp::Tensor> &tensor);

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
MuseBuffer<T>::MuseBuffer(unsigned int height, unsigned int width, std::shared_ptr<kp::Tensor> &tensor)
: m_height(height), m_width(width), m_tensor(tensor) {
}

#endif //MUSECPP_MUSEBUFFER_H
