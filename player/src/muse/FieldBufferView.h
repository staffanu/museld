//
// Created by staffanu on 4/9/23.
//

#ifndef MUSECPP_FIELDBUFFERVIEW_H
#define MUSECPP_FIELDBUFFERVIEW_H

#include <cstdint>
#include "musevk/CommandBuffer.h"
#include "musevk/VulkanBuffer.h"
#include "ControlSignalDecoder.h"

class Logger;

class FieldBufferView {
public:
    FieldBufferView(Logger &log, int frame_no, std::shared_ptr<musevk::VulkanBuffer> const &data, int field_parity);

    void set_frame_no(int frame_no);

    // Called when initializing the chain of 3 frames (6 fields) that are used in
    // round-robin order, so that we can easily access the control data for the field.
    void set_prev_field(FieldBufferView *prev_field);

    // To be called each time the underlying data has been updated.
    void ProcessControlData(float const *control_data, std::pair<float, float> const &rescale);

    std::shared_ptr<musevk::VulkanBuffer> getVulkanBuffer();

    // Returns the previous field's decoded control data used for inter-field decisions.
    std::optional<ControlSignalDecoder> const &control_data() const;

    // Returns this field's decoded control data used to determine VITS phase for equalization.
    std::optional<ControlSignalDecoder> const &own_control_data() const;

    int m_frame_no;
    int m_field_parity;

    std::shared_ptr<musevk::VulkanBuffer> m_data;

private:
    Logger &m_log;
    FieldBufferView *m_prev_field; // for control data access
    std::optional<ControlSignalDecoder> m_control;
};

#endif //MUSECPP_FIELDBUFFERVIEW_H
