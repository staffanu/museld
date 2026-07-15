// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_WINDOWED_SINC_H
#define MUSELD_WINDOWED_SINC_H

#include <cmath>
#include <complex>
#include <vector>

// Rectangular-windowed sinc FIR design, equivalent to gr::filter::firdes
// with WIN_RECTANGULAR. Used by the MUSE/NTSC RF demodulators where firdes'
// deep-stopband notches (from the truncated sinc) outperform what equiripple
// gives at the same tap count.
namespace WindowedSinc {

    // ntaps formula matching firdes::compute_ntaps for WIN_RECTANGULAR
    // (window::max_attenuation = 21).
    inline int rectangular_ntaps(double Fs, double transition_width) {
        int n = (int)(21.0 * Fs / (22.0 * transition_width));
        if ((n & 1) == 0) ++n;
        return n;
    }

    // Real low-pass: h[n] = sin(n*w)/(n*pi), with h[0] = w/pi where w = 2*pi*fc/Fs,
    // normalized so the DC gain is exactly 1 (matches firdes, which compensates for
    // truncation ripple). ntaps must be odd.
    template<typename R>
    static std::vector<R> low_pass(int ntaps, double Fs, double cutoff) {
        std::vector<R> h(ntaps);
        const double w = 2.0 * M_PI * cutoff / Fs;
        const int M = (ntaps - 1) / 2;
        double sum = 0.0;
        for (int n = -M; n <= M; ++n) {
            const double v = (n == 0) ? (w / M_PI)
                                      : (std::sin(n * w) / (n * M_PI));
            h[n + M] = R(v);
            sum += v;
        }
        for (auto &x : h) x = R(double(x) / sum);
        return h;
    }

    // Analytic band-pass: real low-pass with cutoff (high-low)/2 heterodyned to
    // fc = (low+high)/2. Heterodyne is centered on the middle tap so the filter
    // stays linear-phase. Note: the demodulator FIR shader correlates rather than
    // convolves, so callers std::reverse() the result before uploading.
    template<typename R>
    static std::vector<std::complex<R>> complex_band_pass(
        int ntaps, double Fs, double low_cutoff, double high_cutoff)
    {
        auto lp = low_pass<double>(ntaps, Fs, (high_cutoff - low_cutoff) / 2.0);
        const double center = (high_cutoff + low_cutoff) / 2.0;
        const int M = (ntaps - 1) / 2;
        std::vector<std::complex<R>> h(ntaps);
        for (int k = 0; k < ntaps; ++k) {
            const double phase = 2.0 * M_PI * center * (k - M) / Fs;
            h[k] = std::complex<R>(R(lp[k] * std::cos(phase)),
                                   R(lp[k] * std::sin(phase)));
        }
        return h;
    }

}

#endif // MUSELD_WINDOWED_SINC_H
