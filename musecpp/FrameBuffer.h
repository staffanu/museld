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
    FrameBuffer(int frame_no, uint8_t *data);
    ~FrameBuffer();

    uint8_t *data();
    std::pair<double, double> estimate_eq();
    void ApplyInverseTransmissionGamma();
    FieldBufferView &get_field(int parity);

private:
    static std::pair<double, double> linear_regression(std::vector<std::pair<double, double>> const &values);
    static std::array<uint8_t, 256> m_inv_gamma_Y;
    static std::array<uint8_t, 256> m_inv_gamma_C;

    int m_frame_no;
    uint8_t *m_data;
    Eigen::Map<FrameMatrix> m_frame_mem;
    std::vector<FieldBufferView> m_fields;
};

#endif //MUSECPP_FRAMEBUFFER_H
