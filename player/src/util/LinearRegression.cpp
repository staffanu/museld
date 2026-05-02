//
// Created by staffanu on 6/22/24.
//

#include <numeric>
#include "LinearRegression.h"

std::pair<float, float> LinearRegression::linearRegression(std::vector<std::pair<float, float>> const &values) {
    float sum_x = std::accumulate(values.begin(), values.end(), 0.0f,
                                  [](float acc, std::pair<float, float> v) { return acc + v.first; });
    float sum_y = std::accumulate(values.begin(), values.end(), 0.0f,
                                  [](float acc, std::pair<float, float> v) { return acc + v.second; });
    float sum_xx = std::accumulate(values.begin(), values.end(), 0.0f,
                                   [](float acc, std::pair<float, float> v) { return acc + v.first * v.first; });
    float sum_xy = std::accumulate(values.begin(), values.end(), 0.0f,
                                   [](float acc, std::pair<float, float> v) { return acc + v.first * v.second; });
    int n = (int)values.size();
    float a = ((float)n * sum_xy - sum_x * sum_y) / ((float)n * sum_xx - sum_x * sum_x);
    float b = (sum_y - a * sum_x) / (float)n;
    return {a, b};
}
