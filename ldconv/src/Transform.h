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
//
// drop_bits then rounds the result to a multiple of 2^drop_bits about the zero
// level, which throws away that many of the least significant bits.  This is
// the one lossy thing ldconv does; it exists because a capture whose lowest
// bits are noise compresses better without them.  Rounding is to nearest, and
// a value that already lands on a step is left alone, so quantizing an
// already-quantized stream changes nothing.
class Transform {
public:
    Transform(int32_t src_zero, int32_t dst_zero, double gain,
              int32_t min_code, int32_t max_code, int drop_bits = 0);

    void apply(int32_t *samples, size_t count);

    double gain() const { return m_gain; }
    uint64_t clipped() const { return m_clipped; }

private:
    enum class Mode { Copy, ShiftLeft, ShiftRight, Scale };

    void quantize(int32_t *samples, size_t count) const;

    Mode m_mode;
    int m_shift = 0;
    double m_gain;
    int32_t m_src_zero;
    int32_t m_dst_zero;
    int32_t m_min;
    int32_t m_max;
    int m_drop = 0;      // 0: keep every bit
    int32_t m_quant_min = 0; // the steps nearest the ends of the range
    int32_t m_quant_max = 0;
    uint64_t m_clipped = 0;
};

#endif //LDCONV_TRANSFORM_H
