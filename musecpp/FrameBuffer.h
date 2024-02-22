//
// Created by staffanu on 4/9/23.
//

#ifndef MUSECPP_FRAMEBUFFER_H
#define MUSECPP_FRAMEBUFFER_H

#include <vector>
#include <cstdint>
#include "musevk/VulkanBuffer.h"

class FieldBufferView;
class Logger;

class FrameBuffer {
public:
    FrameBuffer(Logger &log, int frame_no, std::shared_ptr<musevk::VulkanBuffer> data);

    static std::pair<float, float> EstimateEq(float const *data);

    void set_frame_no(int frame_no);
    std::shared_ptr<musevk::VulkanBuffer> &data();
    FieldBufferView &get_field(int parity);
    void ProcessControlData(float const *frame_data, std::pair<float, float> const &eq);

private:
    static std::pair<float, float> LinearRegression(const std::vector <std::pair<float, float>> &values);

    int m_frame_no;
    std::shared_ptr<musevk::VulkanBuffer> m_data;
    std::vector<FieldBufferView> m_fields;
};

#endif //MUSECPP_FRAMEBUFFER_H
