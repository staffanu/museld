//
// Created by staffanu on 4/9/23.
//

#ifndef MUSECPP_FRAMEBUFFER_H
#define MUSECPP_FRAMEBUFFER_H

#include <vector>
#include <string>
#include <cstdint>
#include <format>
#include "musevk/VulkanBuffer.h"
#include "DiscInfo.h"
#include "DiscCode.h"

class FieldBufferView;
class Logger;

class FrameBuffer {
public:
    FrameBuffer(Logger &log, int frame_no, std::shared_ptr<musevk::VulkanBuffer> data);

    static std::pair<float, float> EstimateEq(float const *data);

    void set_frame_no(int frame_no, long input_offset, double input_samples_per_sample);
    [[nodiscard]] long getInputOffset() const;
    [[nodiscard]] double getInputSamplesPerMuseSample() const;
    std::shared_ptr<musevk::VulkanBuffer> &data();
    FieldBufferView &get_field(int parity);
    [[nodiscard]] std::shared_ptr<DiscCode> getDiscCode() const;
    void ProcessControlData(float const *frame_data, std::pair<float, float> const &eq);
    void processDiscCode();

private:
    int m_frame_no;
    long m_input_offset;
    double m_input_samples_per_sample;
    std::shared_ptr<musevk::VulkanBuffer> m_data;
    std::vector<FieldBufferView> m_fields;
    std::shared_ptr<DiscCode> m_disc_code;
};

#endif //MUSECPP_FRAMEBUFFER_H
