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
    FrameBuffer(int frame_no, std::shared_ptr<MuseBuffer<float>> const &data);

    std::shared_ptr<MuseBuffer<float>> data();
    std::pair<float, float> estimate_eq();
    void ApplyInverseTransmissionGamma();
    FieldBufferView &get_field(int parity);

private:
    static std::pair<float, float> linear_regression(std::vector<std::pair<float, float>> const &values);

    int m_frame_no;
    std::shared_ptr<MuseBuffer<float>> m_data;
    std::vector<FieldBufferView> m_fields;
};

#endif //MUSECPP_FRAMEBUFFER_H
