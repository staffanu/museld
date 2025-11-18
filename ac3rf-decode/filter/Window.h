//
// Created by staffanu on 11/18/25.
//

#ifndef AC3RF_DECODE_WINDOW_H
#define AC3RF_DECODE_WINDOW_H

#include <cmath>
#include <vector>

namespace Window {
    static std::vector<float> raised_cosine(int n, float a0) {
        std::vector<float> w;
        for (int i = 0; i < n; i++)
            w.push_back(a0 - (1 - a0) * cos(2 * M_PI * i / (n - 1)));
        return w;
    }

    static std::vector<float> hamming(int n) {
        return raised_cosine(n, 25.0 / 46);
    }
};

#endif //AC3RF_DECODE_WINDOW_H
