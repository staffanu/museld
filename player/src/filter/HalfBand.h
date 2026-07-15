// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_HALF_BAND_H
#define MUSELD_HALF_BAND_H

#include <cassert>
#include <cmath>
#include <vector>

// Kaiser-windowed half-band lowpass FIR designer with cutoff Fs/4. The
// Kaiser parameter beta picks the stopband attenuation:
//   beta = 0.1102*(A_dB - 8.7)   for A_dB > 50
// e.g. beta=7.86 → ~80 dB, beta=10.06 → ~100 dB.
//
// Length must be 4k+3 (3, 7, 11, 15, 19, 23, 27, ...) so that the
// every-other-zero half-band structure holds exactly. This both halves the
// multiply count when the application exploits the structure and guarantees
// perfect symmetry around Fs/4 (passband ripple == stopband ripple).
namespace HalfBand {

    // Kaiser beta presets from Kaiser's empirical formula
    //   beta = 0.1102 * (A_dB - 8.7)   (A_dB > 50)
    constexpr double kBeta80dB = 7.857;
    constexpr double kBeta100dB = 10.056;

    inline double bessel_i0(double x) {
        // Modified Bessel I0(x) via series. Converges fast for the |x| range
        // we use here (Kaiser beta ≤ ~12 → arg ≤ beta).
        double sum = 1.0, term = 1.0;
        const double y = (x / 2.0) * (x / 2.0);
        for (int k = 1; k < 64; ++k) {
            term *= y / (double(k) * double(k));
            sum += term;
            if (term < 1e-18 * sum) break;
        }
        return sum;
    }

    template<typename R>
    static std::vector<R> kaiser(int ntaps, double beta) {
        assert(ntaps % 4 == 3 && "half-band length must be 4k+3");
        std::vector<R> h(ntaps);
        const int M = (ntaps - 1) / 2;
        const double i0_beta = bessel_i0(beta);
        for (int n = -M; n <= M; ++n) {
            // Ideal half-band sinc h_ideal[n] = sin(n*pi/2) / (n*pi).
            // Even non-zero n gives 0; odd n gives ±1/(n*pi); n=0 gives 1/2.
            double s;
            if (n == 0) {
                s = 0.5;
            } else if (n % 2 == 0) {
                s = 0.0;
            } else {
                s = std::sin(n * M_PI / 2.0) / (n * M_PI);
            }
            const double r = double(n) / double(M);
            const double w = bessel_i0(beta * std::sqrt(1.0 - r * r)) / i0_beta;
            h[n + M] = R(s * w);
        }
        return h;
    }

}

#endif // MUSELD_HALF_BAND_H
