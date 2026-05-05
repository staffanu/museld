//
// Created by staffanu on 4/19/23.
//

#ifndef MUSECPP_CONTROLSIGNALDECODER_H
#define MUSECPP_CONTROLSIGNALDECODER_H

#include <map>
#include <array>
#include <optional>

class Logger;

enum MotionInformation {
    Normal, CompleteStillPicture, SlightlyInMotion, SceneChange, Motion
};

struct ControlSignalDecoder {
    explicit ControlSignalDecoder(Logger &log, float const *data, std::pair<float, float> const &rescale);
    void log_control_data() const;
    std::optional<int> field_subsampling_phase_Y;
    std::optional<int> horizontal_motion_vector;
    std::optional<int> vertical_motion_vector;
    std::optional<int> frame_subsampling_phase_Y;
    std::optional<int> frame_subsampling_phase_C;
    std::optional<int> motion_sensitivity_ctrl;
    std::optional<int> edge_detection_prohibited;
    std::optional<MotionInformation> motion_information;
    std::optional<int> motion_extent;
    std::optional<int> still_picture;

private:
    static std::string motionInformationToString(const MotionInformation &i) {
        switch (i) {
            case MotionInformation::Normal: return "normal";
            case MotionInformation::CompleteStillPicture: return "still picture";
            case MotionInformation::SlightlyInMotion: return "slight motion";
            case MotionInformation::SceneChange: return "scene change";
            case MotionInformation::Motion: return "motion";
            default: throw std::invalid_argument("Invalid value");
        }
    }

    static std::array<std::array<int, 8>, 4> s_H;
    static std::map<std::array<int, 4>, int> s_H_column_index;

    static std::array<int, 4> multiply(std::array<std::array<int, 8>, 4> m, std::array<int, 8> v);
    static bool is_zero(std::array<int, 4> a);

    Logger &m_log;
};


#endif //MUSECPP_CONTROLSIGNALDECODER_H
