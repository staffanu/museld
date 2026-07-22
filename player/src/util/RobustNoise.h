// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_ROBUSTNOISE_H
#define MUSECPP_ROBUSTNOISE_H

#include <algorithm>
#include <cmath>
#include <complex>
#include <valarray>
#include <vector>
#include "filter/FFT.h"

// Robust helpers for estimating channel noise on flat reference regions of a
// video frame (used by the MUSE and NTSC noise measurements).
class RobustNoise {
public:
    // Approximate median via nth_element; exact enough for noise estimation.
    // Note: partially reorders v — pass a scratch copy if the order matters.
    static float median(std::vector<float> &v) {
        auto mid = v.begin() + v.size() / 2;
        std::nth_element(v.begin(), mid, v.end());
        return *mid;
    }

    // Appends the residuals of a robust line fit over the window to out.  Removing
    // level and tilt keeps low-frequency wander (DPLL residuals, hum) out of the
    // noise estimate, and the median-based fit keeps a dropout spike in the window
    // from dragging the fit and inflating every residual (a least-squares fit does).
    static void appendDetrendedResiduals(float const *samples, int count, std::vector<float> &out,
                                         float *center_out = nullptr) {
        int half = count / 2;
        std::vector<float> tmp(half);
        for (int i = 0; i < half; i++)
            tmp[i] = (samples[i + half] - samples[i]) / (float)half;
        float slope = median(tmp);
        size_t base = out.size();
        for (int i = 0; i < count; i++)
            out.push_back(samples[i] - slope * (i - (count - 1) / 2.0f));
        tmp.assign(out.begin() + base, out.end());
        float center = median(tmp); // window level at the window centre
        if (center_out != nullptr)
            *center_out = center;
        for (size_t i = base; i < out.size(); i++)
            out[i] -= center;
    }

    // Median absolute deviation scaled to equal σ for Gaussian noise; unlike an
    // RMS it is not thrown off by a dropout spike in the window.
    static float robustSigma(std::vector<float> &residuals) {
        for (float &r : residuals)
            r = std::abs(r);
        return median(residuals) * 1.4826f;
    }

    // Detrends the window, applies a Hann window, and accumulates the power
    // spectrum into psd[count] (count must be a power of two).  For white noise
    // of variance σ² every bin converges to σ², so mean-over-bins recovers the
    // total power (Parseval).
    static void accumulateDetrendedWindowPsd(float const *samples, int count, double *psd) {
        std::vector<float> residuals;
        residuals.reserve(count);
        appendDetrendedResiduals(samples, count, residuals);
        std::valarray<std::complex<double>> x(count);
        double window_power = 0;
        for (int i = 0; i < count; i++) {
            double w = 0.5 - 0.5 * cos(2 * M_PI * i / count); // Hann
            window_power += w * w;
            x[i] = residuals[i] * w;
        }
        FFT<double>::fft(x);
        for (int i = 0; i < count; i++)
            psd[i] += std::norm(x[i]) / window_power;
    }
};

#endif //MUSECPP_ROBUSTNOISE_H
