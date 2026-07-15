// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_RRC_H
#define AC3RF_DECODE_RRC_H

#include <cmath>
#include <vector>

namespace RaisedCosine {
    // The "communications sinc"
    static double sinc(double x) {
        return x == 0 ? 1 : sin(M_PI * x) / (M_PI * x);
    }

    static std::vector<double> rcFilter(double samples_per_symbol, int  span, double beta) {
        std::vector<double> h;
        const int n = ceil(span * samples_per_symbol) * 2 + 1;
        const double T = samples_per_symbol;

        double max = 0;
        for (double t = -(n / 2); t <= n / 2; t++) {
            double y;
            if (fabs(fabs(2 * beta * t / T) - 1) < 1e-10)
                y = M_PI / (4 * T) * sinc(1 / (2 * beta));
            else
                y = 1 / T * sinc(t / T) * cos(M_PI * beta * t / T) / (1 - pow(2 * beta * t / T, 2));

            h.push_back(y);
            max = std::max(max, y);
        }

        for (int i = 0; i < h.size(); i++)
            h[i] /= max;

        return h;
    }

    enum class Normalization {
        eUnitEnergy,    // sum(h^2) = 1 (matched-filter convention; cascaded RRC->RC has unity gain)
        eUnityDcGain,   // sum(h) = 1 (matches gr::filter::firdes::root_raised_cosine)
    };

    // Root-raised cosine FIR. ntaps is forced odd. samples_per_symbol need not
    // be an integer.
    static std::vector<double> rrcFilter(int ntaps, double samples_per_symbol, double beta,
                                         Normalization norm) {
        if ((ntaps & 1) == 0) ++ntaps;
        std::vector<double> h(ntaps);
        const double T = samples_per_symbol;
        const int M = ntaps / 2;

        for (int i = 0; i < ntaps; ++i) {
            const double t = i - M;
            double y;
            if (t == 0)
                y = 1 / T * (1 + beta * (4 / M_PI - 1));
            else if (fabs(fabs(4 * beta * t / T) - 1) < 1e-10)
                y = beta / (T * sqrt(2)) * ((1 + 2 / M_PI) * sin(M_PI / (4 * beta)) + (1 - 2 / M_PI) * cos(M_PI / (4 * beta)));
            else
                y = 1 / T * (sin(M_PI * t / T * (1 - beta)) + 4 * beta * t / T * cos(M_PI * t / T * (1 + beta)))
                    / (M_PI * t / T * (1 - pow(4 * beta * t / T, 2)));
            h[i] = y;
        }

        double scale = 0;
        if (norm == Normalization::eUnitEnergy) {
            for (double v : h) scale += v * v;
            scale = sqrt(scale);
        } else {
            for (double v : h) scale += v;
        }
        for (double &v : h) v /= scale;
        return h;
    }
};

#endif //AC3RF_DECODE_RRC_H
