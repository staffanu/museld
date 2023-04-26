//
// Created by staffanu on 4/19/23.
//

#ifndef MUSECPP_CONTROLSIGNALDECODER_H
#define MUSECPP_CONTROLSIGNALDECODER_H

#include <map>
#include <optional>
#include "Eigen/Dense"
#include "MuseTypes.h"

enum MotionInformation {
    Normal, CompleteStillPicture, SlightlyInMotion, SceneChange, Motion
};

struct ControlSignalDecoder {
    explicit ControlSignalDecoder(MappedFrameMatrix const &data);
    std::optional<int> field_subsampling_phase_Y;
    std::optional<int> horizontal_motion_vector;
    std::optional<int> vertical_motion_vector;
    std::optional<int> frame_subsampling_phase_Y;
    std::optional<int> frame_subsampling_phase_C;
    std::optional<MotionInformation> motion_information;
    std::optional<int> motion_extent;

private:
    static Eigen::Matrix<int, 4, 8> s_H;
    static std::map<Eigen::Vector4i, int> s_H_column_index;
};


#endif //MUSECPP_CONTROLSIGNALDECODER_H
