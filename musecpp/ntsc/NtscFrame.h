//
// Created by staffanu on 6/22/24.
//

#ifndef MUSECPP_NTSCFRAME_H
#define MUSECPP_NTSCFRAME_H


#include "util/Logger.h"
#include "musevk/VulkanManager.h"
#include "NtscFieldView.h"

class NtscFrame {
public:
    NtscFrame(Logger &log, int frame_no, musevk::VulkanManager &manager);

    void set_frame_no(int frame_no, long input_offset, double input_samples_per_sample);
    [[nodiscard]] long getInputOffset() const;
    [[nodiscard]] double getInputSamplesPerNtscSample() const;
    std::shared_ptr<musevk::VulkanBuffer> &data();
    NtscFieldView &get_field(int parity);

private:
    int m_frame_no;
    long m_input_offset;
    double m_input_samples_per_sample;
    std::shared_ptr<musevk::VulkanBuffer> m_data;
    std::vector<NtscFieldView> m_fields;
};


#endif //MUSECPP_NTSCFRAME_H
