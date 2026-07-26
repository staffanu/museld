// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef LDCONV_TRANSFORM_H
#define LDCONV_TRANSFORM_H

#include <cstddef>
#include <cstdint>

// Moves code values from the source's domain into the destination's:
//
//     out = clamp(round((in - src_zero) * gain) + dst_zero)
//
// The default gain lines the two full-scale ranges up, so 10-bit .lds and
// 16-bit .s16 are related by the factor of 64 that ld-decode's own tools use.
// Powers of two, which is every default, are done in integer arithmetic and
// are therefore exact and reversible.
class Transform {
public:
    Transform(int32_t src_zero, int32_t dst_zero, double gain, int32_t min_code, int32_t max_code);

    void apply(int32_t *samples, size_t count);

    double gain() const { return m_gain; }
    uint64_t clipped() const { return m_clipped; }

private:
    enum class Mode { Copy, ShiftLeft, ShiftRight, Scale };

    Mode m_mode;
    int m_shift = 0;
    double m_gain;
    int32_t m_src_zero;
    int32_t m_dst_zero;
    int32_t m_min;
    int32_t m_max;
    uint64_t m_clipped = 0;
};

#endif //LDCONV_TRANSFORM_H
