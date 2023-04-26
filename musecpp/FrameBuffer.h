//
// Created by staffanu on 4/9/23.
//

#ifndef MUSECPP_FRAMEBUFFER_H
#define MUSECPP_FRAMEBUFFER_H

#include <vector>
#include <cstdint>
#include "Eigen/Dense"
#include "MuseTypes.h"
#include "FieldBufferView.h"

class FrameBuffer {
public:
    FrameBuffer(int frame_no, DecoderInt *data);
    ~FrameBuffer();

    DecoderInt *data();
    std::pair<double, double> estimate_eq();
    void ApplyInverseTransmissionGamma();
    FieldBufferView &get_field(int parity);

private:
    int _frame_no;
    DecoderInt *_data;
    Eigen::Map<FrameMatrix> _frame_mem;
    std::vector<FieldBufferView> _fields;
};

#endif //MUSECPP_FRAMEBUFFER_H
