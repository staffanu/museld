// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_KAISER_LOW_PASS_H
#define MUSELD_KAISER_LOW_PASS_H

#include <cassert>
#include <cmath>
#include <vector>

#include "HalfBand.h"

// Kaiser-windowed sinc low-pass FIR designer with an arbitrary cutoff, for the
// stages where HalfBand's fixed Fs/4 cutoff does not fit (channel selection,
// audio-band filtering).  The tap count follows Kaiser's empirical formula from
// the requested stopband attenuation and transition width, and the result is
// normalized to unit DC gain.
namespace KaiserLowPass {

    // Kaiser's tap count estimate N ≈ (A - 7.95) / (2.285 * dw), rounded up to odd.
    inline int ntapsFor(double Fs, double transition_width, double attenuation_db) {
        const double dw = 2.0 * M_PI * transition_width / Fs;
        int n = (int)std::ceil((attenuation_db - 7.95) / (2.285 * dw));
        if ((n & 1) == 0) ++n;
        return n;
    }

    // Kaiser's beta for a given stopband attenuation (valid above 50 dB).
    inline double betaFor(double attenuation_db) {
        assert(attenuation_db > 50);
        return 0.1102 * (attenuation_db - 8.7);
    }

    // The cutoff is the -6 dB point; the stopband starts at cutoff + transition_width / 2.
    template<typename R>
    static std::vector<R> design(double Fs, double cutoff, double transition_width, double attenuation_db) {
        const int ntaps = ntapsFor(Fs, transition_width, attenuation_db);
        const double beta = betaFor(attenuation_db);
        const int M = (ntaps - 1) / 2;
        const double w = 2.0 * M_PI * cutoff / Fs;
        const double i0_beta = HalfBand::bessel_i0(beta);

        std::vector<R> h(ntaps);
        double sum = 0.0;
        for (int n = -M; n <= M; ++n) {
            const double s = (n == 0) ? (w / M_PI) : (std::sin(n * w) / (n * M_PI));
            const double r = double(n) / double(M);
            const double v = s * HalfBand::bessel_i0(beta * std::sqrt(1.0 - r * r)) / i0_beta;
            h[n + M] = R(v);
            sum += v;
        }
        for (auto &x : h)
            x = R(double(x) / sum);
        return h;
    }

}

#endif // MUSELD_KAISER_LOW_PASS_H
