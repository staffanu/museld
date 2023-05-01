//
// Created by staffanu on 4/9/23.
//

#ifndef MUSECPP_FIELDBUFFERVIEW_H
#define MUSECPP_FIELDBUFFERVIEW_H

#include <cstdint>
#include "Eigen/Dense"
#include "MuseTypes.h"
#include "ControlSignalDecoder.h"

class FieldBufferView {
public:
    FieldBufferView(int frame_no, uint8_t *data, int field_parity);

    // Intended to be called immediately after construction.  The reason this is not a constructor
    // parameter is that we call this from the main loop after creating the FrameBuffer.
    void ProcessControlData(MappedFrameMatrix const &control_data);

    MappedFrameMatrix control_data_buffer();
    MappedFrameMatrix frame_buffer_Y();
    MappedFrameMatrix frame_buffer_C();
    MappedFrameMatrix audio_buffer();

    int m_frame_no;
    int m_field_parity;
    std::optional<ControlSignalDecoder> m_control;

private:
    uint8_t *m_data;
};

#endif //MUSECPP_FIELDBUFFERVIEW_H
