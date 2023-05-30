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
#include "VulkanResources.h"

class FieldBufferView {
public:
    FieldBufferView(int frame_no, std::shared_ptr<MuseBuffer<float>> const &data, int field_parity);

    // Intended to be called immediately after construction.  The reason this is not a constructor
    // parameter is that we call this from the main loop after creating the FrameBuffer.
    void ProcessControlData(MuseSubBuffer const &control_data);

    std::shared_ptr<musevk::Tensor> tensor();

    MuseSubBuffer control_data_buffer() const;
    MuseSubBuffer audio_buffer() const;

    int m_frame_no;
    int m_field_parity;
    std::optional<ControlSignalDecoder> m_control;

//private:
    std::shared_ptr<MuseBuffer<float>> m_data;
};

#endif //MUSECPP_FIELDBUFFERVIEW_H
