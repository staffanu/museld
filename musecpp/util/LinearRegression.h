//
// Created by staffanu on 6/22/24.
//

#ifndef MUSECPP_LINEARREGRESSION_H
#define MUSECPP_LINEARREGRESSION_H

#include <vector>

class LinearRegression {
public:
    static std::pair<float, float> linearRegression(const std::vector <std::pair<float, float>> &values);
};


#endif //MUSECPP_LINEARREGRESSION_H
