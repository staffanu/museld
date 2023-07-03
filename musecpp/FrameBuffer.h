//
// Created by staffanu on 4/9/23.
//

#ifndef MUSECPP_FRAMEBUFFER_H
#define MUSECPP_FRAMEBUFFER_H

#include <vector>
#include <cstdint>
#include "MuseTypes.h"
#include "FieldBufferView.h"
#include "MuseBuffer.h"

class FrameBuffer {
public:
    FrameBuffer(Logger &log, int frame_no, MuseBuffer data);

    static std::pair<float, float> EstimateEq(uint16_t const *data);

    void set_frame_no(int frame_no);
    MuseBuffer &data();
    FieldBufferView &get_field(int parity);
    void ProcessControlData(uint16_t const *frame_data, std::pair<float, float> const &eq);

private:
    static std::pair<float, float> LinearRegression(const std::vector <std::pair<float, float>> &values);

    int m_frame_no;
    MuseBuffer m_data;
    std::vector<FieldBufferView> m_fields;
};

#endif //MUSECPP_FRAMEBUFFER_H
