// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#ifndef MUSELD_INTERPOLATE_H
#define MUSELD_INTERPOLATE_H

// Cubic interpolation through 4 equally-spaced points b0..b3.
// frac = fractional position ∈ [0, 1) between b1 and b2.
inline float cubicInterpolate(float b0, float b1, float b2, float b3, float frac) {
    float x = 1.0f + frac;
    float a0 = b0;
    float a1 = b1 - a0;
    float a2 = (b2 - a0 - 2.f * a1) * 0.5f;
    float a3 = (b3 - a0 - 3.f * a1 - 6.f * a2) * (1.f / 6.f);
    return a0 + x * (a1 + (x - 1.f) * (a2 + (x - 2.f) * a3));
}

#endif //MUSELD_INTERPOLATE_H
