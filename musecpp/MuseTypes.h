//
// Created by staffanu on 4/17/23.
//

#ifndef MUSECPP_MUSETYPES_H
#define MUSECPP_MUSETYPES_H

#include <cstdint>
#include <optional>
#include "Eigen/Dense"

// input file currently uses shorts in the range 0...1023
#define MUSE_INPUT_MULT 4

typedef Eigen::Array<uint8_t, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> FrameMatrix;
typedef Eigen::Map<FrameMatrix, Eigen::Unaligned, Eigen::Stride<Eigen::Dynamic, Eigen::Dynamic>> MappedFrameMatrix;

#define FRAME_STRIDE Eigen::Stride<Eigen::Dynamic, Eigen::Dynamic>(480, 1)
#define MUSE_BUF_HEIGHT 516
#define MUSE_Y_BUF_WIDTH 374
#define MUSE_C_BUF_WIDTH 94

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

template<typename T>
std::ostream& operator<<(std::ostream& os, std::optional<T> const& opt)
{
    return opt ? os << opt.value() : os << "?";
}

#endif //MUSECPP_MUSETYPES_H
