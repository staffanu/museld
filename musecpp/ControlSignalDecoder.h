//
// Created by staffanu on 4/19/23.
//

#ifndef MUSECPP_CONTROLSIGNALDECODER_H
#define MUSECPP_CONTROLSIGNALDECODER_H

#include <map>
#include <array>
#include <optional>
#include "MuseTypes.h"
#include "Logger.h"

enum MotionInformation {
    Normal, CompleteStillPicture, SlightlyInMotion, SceneChange, Motion
};

struct ControlSignalDecoder {
    explicit ControlSignalDecoder(Logger &log, uint16_t const *data, std::pair<float, float> const &eq);
    void log_control_data() const;
    std::optional<int> field_subsampling_phase_Y;
    std::optional<int> horizontal_motion_vector;
    std::optional<int> vertical_motion_vector;
    std::optional<int> frame_subsampling_phase_Y;
    std::optional<int> frame_subsampling_phase_C;
    std::optional<MotionInformation> motion_information;
    std::optional<int> motion_extent;

private:
    static std::array<std::array<int, 8>, 4> s_H;
    static std::map<std::array<int, 4>, int> s_H_column_index;

    static std::array<int, 4> multiply(std::array<std::array<int, 8>, 4> m, std::array<int, 8> v);
    static bool is_zero(std::array<int, 4> a);

    Logger &m_log;
};


#endif //MUSECPP_CONTROLSIGNALDECODER_H
