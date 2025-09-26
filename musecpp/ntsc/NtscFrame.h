//
// Created by staffanu on 6/22/24.
//

#ifndef MUSECPP_NTSCFRAME_H
#define MUSECPP_NTSCFRAME_H


#include "util/Logger.h"
#include "musevk/VulkanManager.h"
#include "NtscFieldView.h"
#include "VbiData.h"

class NtscFrame {
public:
    NtscFrame(Logger &log, int frame_no, musevk::VulkanManager &manager);

    void set_frame_no(int frame_no, long input_offset, double input_samples_per_sample);
    [[nodiscard]] long getInputOffset() const;
    [[nodiscard]] double getInputSamplesPerNtscSample() const;
    std::shared_ptr<musevk::VulkanBuffer> &data();
    std::shared_ptr<musevk::VulkanBuffer> &y_data();
    std::shared_ptr<musevk::VulkanBuffer> &burst_phase_data();
    std::shared_ptr<musevk::VulkanBuffer> &c_data();
    NtscFieldView &get_field(int parity);
    [[nodiscard]] std::shared_ptr<VbiData> getVbiData() const;
    void processVbi();

private:
    int processVbiLine(int line);

    int m_frame_no;
    long m_input_offset;
    double m_input_samples_per_sample;
    std::shared_ptr<musevk::VulkanBuffer> m_data;
    std::shared_ptr<musevk::VulkanBuffer> m_burst_phase_data;
    std::shared_ptr<musevk::VulkanBuffer> m_y_data; // Filtered by notch filter
    std::shared_ptr<musevk::VulkanBuffer> m_c_data; // Filtered by bandpass filter
    std::vector<NtscFieldView> m_fields;
    std::shared_ptr<VbiData> m_vbi_data;
};


#endif //MUSECPP_NTSCFRAME_H
