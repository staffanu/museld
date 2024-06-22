//
// Created by staffanu on 6/22/24.
//

#include "NtscFrame.h"
#include "NtscFieldView.h"
#include "NtscInputBlock.h"
#include "musevk/VulkanBuffer.h"
#include "musevk/VulkanManager.h"

NtscFrame::NtscFrame(Logger &log, int frame_no, musevk::VulkanManager &manager)
: m_frame_no(frame_no),
        m_input_offset(-1),
        m_input_samples_per_sample(0),
        m_data(std::make_unique<musevk::VulkanBuffer>(
                manager, musevk::Size(NtscInputBlock::c_samples_per_video_line, NtscInputBlock::c_total_video_lines), 2 /* sizeof(float16) */,
                vk::BufferUsageFlagBits::eStorageBuffer, musevk::eHostRead)),
        m_fields({NtscFieldView(log, frame_no, m_data, 0), NtscFieldView(log, frame_no, m_data, 1) }) {
}

void NtscFrame::set_frame_no(int frame_no, long input_offset, double input_samples_per_sample) {
    m_frame_no = frame_no;
    m_input_offset = input_offset;
    m_input_samples_per_sample = input_samples_per_sample;
    m_fields[0].set_frame_no(frame_no);
    m_fields[1].set_frame_no(frame_no);
}

long NtscFrame::getInputOffset() const {
    return m_input_offset;
}

double NtscFrame::getInputSamplesPerMuseSample() const {
    return m_input_samples_per_sample;
}

std::shared_ptr<musevk::VulkanBuffer> &NtscFrame::data() {
    return m_data;
}

NtscFieldView &NtscFrame::get_field(int parity) {
    return m_fields[parity];
}
