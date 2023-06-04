//
// Created by staffanu on 5/14/23.
//

#ifndef MUSECPP_MUSESUBBUFFER_H
#define MUSECPP_MUSESUBBUFFER_H

#include "MuseTypes.h"
#include "MuseBuffer.h"

class MuseSubBuffer {
public:
    MuseSubBuffer(MuseBuffer &data,
                  unsigned int y_offset, unsigned int x_offset,
                  unsigned int height, unsigned int width);

    const float *operator [](size_t row) const
    {
        assert(row < m_height);
        return m_data.getVulkanBuffer()->data<float>() + (row + m_y_offset) * MUSE_TOTAL_WIDTH + m_x_offset;
    }

    float *operator [](size_t row)
    {
        assert(row < m_height);
        return m_data.getVulkanBuffer()->data<float>() + (row + m_y_offset) * MUSE_TOTAL_WIDTH + m_x_offset;
    }

private:
    MuseBuffer &m_data;
    unsigned int m_y_offset;
    unsigned int m_x_offset;
    unsigned int m_height;
    unsigned int m_width;
};

#endif //MUSECPP_MUSESUBBUFFER_H
