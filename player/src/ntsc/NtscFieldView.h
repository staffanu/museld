// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_NTSCFIELDVIEW_H
#define MUSECPP_NTSCFIELDVIEW_H

#include "musevk/VulkanBuffer.h"
#include "logging/Logger.h"

class NtscFieldView {
public:
    NtscFieldView(Logger &log, int frame_no, std::shared_ptr<musevk::VulkanBuffer> const &data,
    std::shared_ptr<musevk::VulkanBuffer> const &burst_phase_data,
    int field_parity);

    void set_frame_no(int frame_no) {}
    // Called when initializing the chain of frames so that we can easily find
    // the field what is interlaces with this one when decoding at 60 fps.
    void set_prev_field(NtscFieldView *prev_field) {}

    int m_frame_no;
    int m_field_parity;

    std::shared_ptr<musevk::VulkanBuffer> m_data;
    std::shared_ptr<musevk::VulkanBuffer> m_burst_phase_data;

private:
    Logger &m_log;
    NtscFieldView *m_prev_field; // for control data access
};



#endif //MUSECPP_NTSCFIELDVIEW_H
