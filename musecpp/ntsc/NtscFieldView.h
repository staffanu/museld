//
// Created by staffanu on 6/22/24.
//

#ifndef MUSECPP_NTSCFIELDVIEW_H
#define MUSECPP_NTSCFIELDVIEW_H

#include "musevk/VulkanBuffer.h"
#include "util/Logger.h"

class NtscFieldView {
public:
    NtscFieldView(Logger &log, int frame_no, std::shared_ptr<musevk::VulkanBuffer> const &data,
    std::shared_ptr<musevk::VulkanBuffer> const &burst_phase_data,
    std::shared_ptr<musevk::VulkanBuffer> const &y_data, std::shared_ptr<musevk::VulkanBuffer> const &c_data,
    int field_parity);

    void set_frame_no(int frame_no) {}
    // Called when initializing the chain of frames so that we can easily find
    // the field what is interlaces with this one when decoding at 60 fps.
    void set_prev_field(NtscFieldView *prev_field) {}

    int m_frame_no;
    int m_field_parity;

    std::shared_ptr<musevk::VulkanBuffer> m_data;
    std::shared_ptr<musevk::VulkanBuffer> m_burst_phase_data;
    std::shared_ptr<musevk::VulkanBuffer> m_y_data;
    std::shared_ptr<musevk::VulkanBuffer> m_c_data;

private:
    Logger &m_log;
    NtscFieldView *m_prev_field; // for control data access
};



#endif //MUSECPP_NTSCFIELDVIEW_H
