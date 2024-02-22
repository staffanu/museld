//
// Created by staffanu on 4/19/23.
//

#include <vector>
#include <cassert>
#include <sstream>
#include <fmt/format.h>
#include "util/Logger.h"
#include "MuseTypes.h"
#include "ControlSignalDecoder.h"

using namespace std;

array<array<int, 8>, 4> ControlSignalDecoder::s_H = [] {
    return array<array<int, 8>, 4>{
            1, 1, 1, 0, 1, 0, 0, 0,
            0, 1, 1, 1, 0, 1, 0, 0,
            1, 1, 0, 1, 0, 0, 1, 0,
            1, 0, 1, 1, 0, 0, 0, 1};
}();

map<array<int, 4>, int> ControlSignalDecoder::s_H_column_index = [] {
    auto ix = map<array<int, 4>, int>();
    for (int col_ix = 0; col_ix < 8; col_ix++) {
        std::array<int, 4> column{};
        for (int i = 0; i < 4; i++)
            column[i] = s_H[i][col_ix];
        ix[column] = col_ix;
    }
    return ix;
}();

ControlSignalDecoder::ControlSignalDecoder(Logger &log, float const *data, std::pair<float, float> const &eq)
: m_log(log) {
    auto groups = vector<pair<array<int, 4>, bool>>();
    groups.reserve(25);
    for (int row = 0; row < 5; row++) {
        for (int col = 7; col < 87; col += 16) { // start of each encoded 4 bit group
            array<int, 8> bits{};
            for (int bit_ix = 0; bit_ix < 8; bit_ix++) {
                float d1 = clamp((data[row * MUSE_TOTAL_WIDTH + col + 2 * bit_ix] - eq.second) / eq.first, 0.0f, 255.0f);
                float d2 = clamp((data[row * MUSE_TOTAL_WIDTH + col + 2 * bit_ix + 1] - eq.second) / eq.first, 0.0f, 255.0f);
                bits[bit_ix] = d1 + d2 > 256 ? 1 : 0;
            }

            array<int, 4> syndrome = multiply(s_H, bits);
            bool is_ok = false;
            if (is_zero(syndrome)) { // no errors
                is_ok = true;
            } else {
                auto error_pos = s_H_column_index.find(syndrome);
                if (error_pos != s_H_column_index.cend()) {
                    bits[error_pos->second] ^= 1;
                    is_ok = true;
                }
            }
            std::array<int, 4> first_4{};
            for (int i = 0; i < 4; i++)
                first_4[i] = bits[i];
            groups.emplace_back(first_4, is_ok);
        }
    }
    assert(groups.size() == 25);

    vector<pair<bool, bool>> result; // pair is (bit value, valid flag)
    result.reserve(32);
    for (int group_ix = 0; group_ix < 8; group_ix++) {
        auto result_freqs = map<array<int, 4>, int>();
        for (int i = 0; i < 3; i++) {
            auto g = groups[group_ix + i * 8];
            if (g.second)
                result_freqs[g.first]++;
        }
        if (result_freqs.empty()) {
            for (int i = 0; i < 4; i++)
                result.emplace_back(false, false);
        } else if (result_freqs.size() == 1) {
            auto r = result_freqs.cbegin()->first;
            for (int i = 0; i < 4; i++)
                result.emplace_back(r[i], true);
        } else {
            auto max_freq = find_if(
                    result_freqs.cbegin(), result_freqs.cend(),
                    [](const pair<array<int, 4>, int> &t) -> bool { return t.second == 2; });
            if (max_freq == result_freqs.end()) {
                for (int i = 0; i < 4; i++)
                    result.emplace_back(false, false);
            } else {
                auto majority = max_freq->first;
                for (int i = 0; i < 4; i++)
                    result.emplace_back(majority[i], true);
            }
        }
    }

    if (m_log.isEnabled(eDebug, eDecoder)) {
        ostringstream ss;
        for (int i = 0; i < 32; i++) {
            if (i != 0 && i % 8 == 0) ss << " ";
            ss << (result[i].second ? result[i].first ? "1" : "0" : "*");
        }
        m_log.debug(eDecoder, ss.str());
    }

    auto vector_index_to_bit_opt = [](const vector<pair<bool, bool>> v, int i) -> optional<int> {
        return v[i].second ? optional(v[i].first) : nullopt;
    };

    auto vector_to_int_opt = [](const vector<pair<bool, bool>> v) -> optional<int> {
        bool valid = all_of(v.cbegin(), v.cend(),
                            [](const pair<bool, bool> &p) -> bool { return p.second; });
        if (valid) {
            int s = 0;
            for (int i = 0; i < v.size(); i++)
                s += v[i].first ? (1 << i) : 0;
            return {s};
        } else
            return nullopt;
    };

    // notice: all indices are 1 less than in the spec, where bits are numbered from 1
    field_subsampling_phase_Y = vector_index_to_bit_opt(result, 0);
    horizontal_motion_vector = vector_to_int_opt(vector(result.cbegin() + 1, result.cbegin() + 5));
    vertical_motion_vector = vector_to_int_opt(vector(result.cbegin() + 5, result.cbegin() + 8));
    frame_subsampling_phase_Y = vector_index_to_bit_opt(result, 8);
    frame_subsampling_phase_C = vector_index_to_bit_opt(result, 9);
    motion_sensitivity_ctrl = vector_index_to_bit_opt(result, 13);
    edge_detection_prohibited = vector_index_to_bit_opt(result, 14);

    auto motion_int_opt = vector_to_int_opt(vector(result.cbegin() + 15, result.cbegin() + 18));
    if (motion_int_opt.has_value()) {
        switch (motion_int_opt.value()) {
            case 0:
                motion_information = optional(Normal);
                motion_extent = nullopt;
                break;
            case 1:
                motion_information = optional(CompleteStillPicture);
                motion_extent = nullopt;
                break;
            case 2:
                motion_information = optional(SlightlyInMotion);
                motion_extent = nullopt;
                break;
            case 3:
                motion_information = optional(SceneChange);
                motion_extent = nullopt;
                break;
            default:
                motion_information = optional(Motion);
                motion_extent = optional(motion_int_opt.value() - 4);
                break;
        }
    } else {
        motion_information = nullopt;
        motion_extent = nullopt;
    }

    still_picture = vector_index_to_bit_opt(result, 23);

    if (horizontal_motion_vector.has_value() && horizontal_motion_vector.value() != 0 ||
        vertical_motion_vector.has_value() && vertical_motion_vector.value() != 0 ||
        motion_sensitivity_ctrl.has_value() && motion_sensitivity_ctrl.value() != 0 ||
        edge_detection_prohibited.has_value() && motion_sensitivity_ctrl.value() != 0)
        // Of course this is not an error -- but if we see examples we don't want to miss them!
        throw runtime_error("Actually found non-zero motion vector!");
}

void ControlSignalDecoder::log_control_data() const {
    m_log.info(eVideo, fmt::format(
            "phases (fieldY frameY frameC): {}{}{}, "
            "motion vector: ({}, {}), "
            "{}{}"
            "motion: {}, extent: {}, still: {}",
            field_subsampling_phase_Y, frame_subsampling_phase_Y, frame_subsampling_phase_C,
            horizontal_motion_vector, vertical_motion_vector,
            motion_sensitivity_ctrl.has_value() ? motion_sensitivity_ctrl.value() != 0 ? "motion_sensitivity_ctrl set, " : "" : "motion_sensitivity_ctrl unknown, ",
            edge_detection_prohibited.has_value() ? edge_detection_prohibited.value() != 0 ? "edge_detection_prohibited set, " : "" : "edge_detection_prohibited unknown, ",
            motion_information.has_value() ? motionInformationToString(motion_information.value()) : "nullopt", motion_extent,
            still_picture));
//    if (motion_information.has_value() && motion_information.value() == SceneChange)
//        m_log.error(eVideo, "***SCENE CHANGE***");
}

std::array<int, 4> ControlSignalDecoder::multiply(std::array<std::array<int, 8>, 4> m, std::array<int, 8> v) {
    std::array<int, 4> result{};
    for (int i = 0; i < 4; i++) {
        int sum = 0;
        for (int j = 0; j < 8; j++)
            sum += m[i][j] * v[j];
        result[i] = sum % 2;
    }
    return result;
}

bool ControlSignalDecoder::is_zero(std::array<int, 4> a) {
    for (int i = 0; i < 4; i++)
        if (a[i] != 0)
            return false;
    return true;
}
