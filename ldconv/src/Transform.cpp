// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <cmath>
#include "Transform.h"

namespace {

int exactLog2(double value) {
    int exponent = 0;
    const double mantissa = std::frexp(value, &exponent);
    return mantissa == 0.5 ? exponent - 1 : -1000; // -1000: not a power of two
}

} // namespace

Transform::Transform(int32_t src_zero, int32_t dst_zero, double gain,
                     int32_t min_code, int32_t max_code, int drop_bits)
    : m_gain(gain), m_src_zero(src_zero), m_dst_zero(dst_zero),
      m_min(min_code), m_max(max_code) {
    if (drop_bits > 0) {
        m_drop = drop_bits;
        // Rounding may push a sample past the end of the range, so it is
        // clamped to the last whole step instead.  That is the quantizer
        // saturating rather than the conversion clipping, so it is not
        // counted: nothing that fits in the destination is being lost.
        const int32_t step = (int32_t)1 << drop_bits;
        m_quant_max = dst_zero + (int32_t)(((int64_t)max_code - dst_zero) / step) * step;
        m_quant_min = dst_zero - (int32_t)(((int64_t)dst_zero - min_code) / step) * step;
    }

    const int log2_gain = exactLog2(gain);
    if (log2_gain == 0)
        m_mode = src_zero == dst_zero ? Mode::Copy : Mode::ShiftLeft;
    else if (log2_gain > 0 && log2_gain < 31) {
        m_mode = Mode::ShiftLeft;
        m_shift = log2_gain;
    } else if (log2_gain < 0 && log2_gain > -31) {
        m_mode = Mode::ShiftRight;
        m_shift = -log2_gain;
    } else {
        m_mode = Mode::Scale;
    }
}

void Transform::apply(int32_t *samples, size_t count) {
    uint64_t clipped = 0;
    const int32_t lo = m_min, hi = m_max;

    switch (m_mode) {
        case Mode::Copy:
            // Still worth a pass: a wider source can hold values the
            // destination cannot, and those have to be caught and counted.
            for (size_t i = 0; i < count; i++) {
                const int32_t v = samples[i];
                const int32_t c = std::clamp(v, lo, hi);
                clipped += c != v;
                samples[i] = c;
            }
            break;

        case Mode::ShiftLeft:
            for (size_t i = 0; i < count; i++) {
                const int64_t v = (((int64_t)samples[i] - m_src_zero) << m_shift) + m_dst_zero;
                const int64_t c = std::clamp<int64_t>(v, lo, hi);
                clipped += c != v;
                samples[i] = (int32_t)c;
            }
            break;

        case Mode::ShiftRight: {
            // Arithmetic shift after a half-step bias, which rounds halves up
            // and leaves values that came from an exact left shift untouched.
            const int64_t half = (int64_t)1 << (m_shift - 1);
            for (size_t i = 0; i < count; i++) {
                const int64_t v = ((((int64_t)samples[i] - m_src_zero) + half) >> m_shift) + m_dst_zero;
                const int64_t c = std::clamp<int64_t>(v, lo, hi);
                clipped += c != v;
                samples[i] = (int32_t)c;
            }
            break;
        }

        case Mode::Scale:
            for (size_t i = 0; i < count; i++) {
                const double v = std::nearbyint(((double)samples[i] - m_src_zero) * m_gain) + m_dst_zero;
                const double c = std::clamp(v, (double)lo, (double)hi);
                clipped += c != v;
                samples[i] = (int32_t)c;
            }
            break;
    }

    m_clipped += clipped;

    if (m_drop != 0)
        quantize(samples, count);
}

void Transform::quantize(int32_t *samples, size_t count) const {
    // The same half-step bias and arithmetic shift as Mode::ShiftRight, only
    // shifted back up again afterwards so the value stays in the destination's
    // domain: the low bits are zeroed rather than removed.  FLAC notices that
    // a whole frame shares them and stores the sample width less those bits.
    const int64_t half = (int64_t)1 << (m_drop - 1);
    for (size_t i = 0; i < count; i++) {
        const int64_t offset = (int64_t)samples[i] - m_dst_zero;
        const int64_t rounded = (((offset + half) >> m_drop) << m_drop) + m_dst_zero;
        samples[i] = (int32_t)std::clamp<int64_t>(rounded, m_quant_min, m_quant_max);
    }
}
