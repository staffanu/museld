//
// Created by staffanu on 4/19/23.
//

#include <map>
#include <iostream>
#include "Eigen/Dense"
#include "MuseTypes.h"
#include "ControlSignalDecoder.h"

using namespace std;
using namespace Eigen;

Matrix<int, 4, 8> ControlSignalDecoder::s_H = []{
        return (Matrix<int, 4, 8>() <<
                1, 1, 1, 0, 1, 0, 0, 0,
                0, 1, 1, 1, 0, 1, 0, 0,
                1, 1, 0, 1, 0, 0, 1, 0,
                1, 0, 1, 1, 0, 0, 0, 1).finished();
}();

map<Vector4i, int> ControlSignalDecoder::s_H_column_index = [] {
    auto ix = map<Vector4i, int>();
    for (int i = 0; i < 8; i++)
        ix[ControlSignalDecoder::s_H.col(i)] = i;
    return ix;
}();

ControlSignalDecoder::ControlSignalDecoder(const MuseSubBuffer &data) {
    auto groups = vector<pair<Vector4i, bool>>();
    groups.reserve(25);
    for (int row = 0; row < 5; row++) {
        for (int col = 7; col < 87; col += 16) { // start of each encoded 4 bit group
            Vector<int, 8> bits;
            for (int bit_ix = 0; bit_ix < 8; bit_ix++) {
                uint8_t d1 = data[row][col + 2 * bit_ix];
                uint8_t d2 = data[row][col + 2 * bit_ix + 1];
                bits[bit_ix] = d1 + d2 > 256 ? 1 : 0;
            }

            Vector4i syndrome = (s_H * bits).unaryExpr([](const int &x) { return x % 2; });
            bool is_ok = false;
            if (syndrome.isZero()) { // no errors
                is_ok = true;
            } else {
                auto error_pos = s_H_column_index.find(syndrome);
                if (error_pos != s_H_column_index.cend()) {
                    bits(error_pos->second) ^= 1;
                    is_ok = true;
                }
            }
            groups.emplace_back(pair(bits.head(4), is_ok));
        }
    }
    assert(groups.size() == 25);

    vector<pair<bool, bool>> result; // pair is (bit value, valid flag)
    result.reserve(32);
    for (int group_ix = 0; group_ix < 8; group_ix++) {
        auto result_freqs = map<Vector4i, int>();
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
                    [](const pair<Vector4i, int> &t) -> bool { return t.second == 2; });
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
//    for (int i = 0; i < 32; i++)
//        cout << (result[i].second ? result[i].first ? "1" : "0" : "*");
//    cout << "  ";

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
}

void ControlSignalDecoder::print_control_data() {
    cout << "phases (fieldY frameY frameC) = "
         << field_subsampling_phase_Y << frame_subsampling_phase_Y << frame_subsampling_phase_C
         << ", hVector=" << horizontal_motion_vector << ", vVector=" << vertical_motion_vector
         << ", motion=" << motion_information << ", extent=" << motion_extent << endl;

}