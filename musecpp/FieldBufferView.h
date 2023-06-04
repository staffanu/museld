//
// Created by staffanu on 4/9/23.
//

#ifndef MUSECPP_FIELDBUFFERVIEW_H
#define MUSECPP_FIELDBUFFERVIEW_H

#include <cstdint>
#include "MuseTypes.h"
#include "ControlSignalDecoder.h"
#include "MuseBuffer.h"
#include "MuseSubBuffer.h"
#include "musevk/CommandQueue.h"

class FieldBufferView {
public:
    FieldBufferView(int frame_no, MuseBuffer &data, int field_parity);

    void set_frame_no(int frame_no);
    void set_prev_field(FieldBufferView *prev_field);

    // Intended to be called immediately after construction.  The reason this is not a constructor
    // parameter is that we call this from the main loop after creating the FrameBuffer.
    void ProcessControlData(uint16_t const *control_data, std::pair<float, float> const &eq);

    std::shared_ptr<musevk::VulkanBuffer> getVulkanBuffer();
    MuseSubBuffer audio_buffer() const;
    std::optional<ControlSignalDecoder> const &control_data();

    int m_frame_no;
    int m_field_parity;

    MuseBuffer &m_data;

private:
    FieldBufferView *m_prev_field; // for control data access
    std::optional<ControlSignalDecoder> m_control;
};

#endif //MUSECPP_FIELDBUFFERVIEW_H
