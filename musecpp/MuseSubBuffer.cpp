//
// Created by staffanu on 5/14/23.
//

#include "MuseSubBuffer.h"

MuseSubBuffer::MuseSubBuffer(
        std::shared_ptr<MuseBuffer<float>> const &data, unsigned int y_offset,
        unsigned int x_offset, unsigned int height, unsigned int width)
: m_data(data),
  m_y_offset(y_offset),
  m_x_offset(x_offset),
  m_height(height),
  m_width(width) {
}
