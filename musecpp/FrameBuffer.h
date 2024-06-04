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
    struct DiscCode {
        int mode;
        int cadr;
        int fadr1;
        int fadr2;

        [[nodiscard]] bool pf() const { return mode & 0x80; } // true if part of TOC (lead-in)
        [[nodiscard]] bool sz() const { return mode & 0x40; } // true if 20 cm disc
        [[nodiscard]] bool df() const { return mode & 0x20; } // true if CLV
        [[nodiscard]] int chapter() const { return cadr & 0x7f; }
        [[nodiscard]] int frame() const { return fadr1 & 0x1ffff; }
    };

    FrameBuffer(Logger &log, int frame_no, std::shared_ptr<musevk::VulkanBuffer> data);

    static std::pair<float, float> EstimateEq(float const *data);

    void set_frame_no(int frame_no, long input_offset, double input_samples_per_sample);
    [[nodiscard]] long getInputOffset() const;
    [[nodiscard]] double getInputSamplesPerMuseSample() const;
    std::shared_ptr<musevk::VulkanBuffer> &data();
    FieldBufferView &get_field(int parity);
    std::optional<DiscCode> getDiscCode() const;
    void ProcessControlData(float const *frame_data, std::pair<float, float> const &eq);
    void processDiscCode();

private:
    static std::pair<float, float> LinearRegression(const std::vector <std::pair<float, float>> &values);

    int m_frame_no;
    long m_input_offset;
    double m_input_samples_per_sample;
    std::shared_ptr<musevk::VulkanBuffer> m_data;
    std::vector<FieldBufferView> m_fields;
    std::optional<DiscCode> m_discCode;
};

#endif //MUSECPP_FRAMEBUFFER_H
