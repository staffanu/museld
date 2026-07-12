// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#ifndef MUSECPP_LINEARREGRESSION_H
#define MUSECPP_LINEARREGRESSION_H

#include <vector>

class LinearRegression {
public:
    static std::pair<float, float> linearRegression(const std::vector <std::pair<float, float>> &values);
};


#endif //MUSECPP_LINEARREGRESSION_H
