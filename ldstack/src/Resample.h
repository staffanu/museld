// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef LDSTACK_RESAMPLE_H
#define LDSTACK_RESAMPLE_H

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

// Lanczos-4 interpolation with a phase table
struct Resampler {
    static constexpr int c_taps = 8, c_phases = 256;
    std::vector<float> table; // [phase][tap]
    Resampler() {
        table.resize(c_phases * c_taps);
        for (int p = 0; p < c_phases; p++) {
            const double frac = (double)p / c_phases;
            double sum = 0;
            for (int t = 0; t < c_taps; t++) {
                const double x = (t - c_taps / 2 + 1) - frac; // distance from the sample point
                double w;
                if (x == 0) w = 1;
                else if (std::fabs(x) >= c_taps / 2) w = 0;
                else {
                    const double a = c_taps / 2;
                    w = a * std::sin(std::numbers::pi * x) * std::sin(std::numbers::pi * x / a) /
                        (std::numbers::pi * std::numbers::pi * x * x);
                }
                table[p * c_taps + t] = (float)w;
                sum += w;
            }
            for (int t = 0; t < c_taps; t++) table[p * c_taps + t] = (float)(table[p * c_taps + t] / sum);
        }
    }
    // Value at (integer position of `in`) + frac; `in` must have c_taps/2 - 1
    // valid samples before it and c_taps/2 after
    float at(const float *in, double frac) const {
        const int p = std::min(c_phases - 1, (int)(frac * c_phases + 0.5));
        const float *w = table.data() + p * c_taps;
        float acc = 0;
        for (int t = 0; t < c_taps; t++) acc += w[t] * in[t - c_taps / 2 + 1];
        return acc;
    }
    // Shift a whole line by `delay` samples (out[i] = in[i + delay]); samples
    // that would come from outside are taken from the edge
    void shift(const float *in, int n, double delay, float *out) const {
        for (int i = 0; i < n; i++) {
            const double pos = i + delay;
            const int64_t ip = (int64_t)std::floor(pos);
            if (ip < c_taps / 2 || ip + c_taps / 2 >= n) {
                out[i] = in[std::clamp<int64_t>(ip, 0, n - 1)];
                continue;
            }
            out[i] = at(in + ip, pos - ip);
        }
    }
};

#endif //LDSTACK_RESAMPLE_H
