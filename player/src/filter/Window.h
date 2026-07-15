// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_WINDOW_H
#define AC3RF_DECODE_WINDOW_H

#include <cmath>
#include <vector>

namespace Window {
    static std::vector<double> raisedCosine(int n, double a0) {
        std::vector<double> w;
        for (int i = 0; i < n; i++)
            w.push_back(a0 - (1 - a0) * cos(2 * M_PI * i / (n - 1)));
        return w;
    }

    static std::vector<double> hamming(int n) {
        return raisedCosine(n, 25.0 / 46);
    }
};

#endif //AC3RF_DECODE_WINDOW_H
