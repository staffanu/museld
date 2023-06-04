//
// Created by staffanu on 4/19/23.
//

#ifndef MUSECPP_CONTROLSIGNALDECODER_H
#define MUSECPP_CONTROLSIGNALDECODER_H

#include <map>
#include <optional>
#include "Eigen/Dense"
#include "MuseTypes.h"
#include "MuseSubBuffer.h"

namespace std
{
    template<> struct less<Eigen::Vector<int, 4>> {
        bool operator()(Eigen::Vector<int, 4> const &a, Eigen::Vector<int, 4> const &b) const {
            assert(a.size() == b.size());
            for (Eigen::Index i = 0; i < a.size(); ++i) {
                if (a[i] < b[i]) return true;
                if (a[i] > b[i]) return false;
            }
            return false;
        }
    };
}

enum MotionInformation {
    Normal, CompleteStillPicture, SlightlyInMotion, SceneChange, Motion
};

struct ControlSignalDecoder {
    explicit ControlSignalDecoder(uint16_t const *data, std::pair<float, float> const &eq);
    void print_control_data() const;
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
