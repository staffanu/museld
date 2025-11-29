//
// Created by Staffan Ulfberg on 11/25/25.
//

#ifndef AC3RF_DECODE_IRRFILTER_H
#define AC3RF_DECODE_IRRFILTER_H

#include <vector>
#include <deque>
#include <string>

class IirFilter {
public:
    IirFilter(const std::string name, const std::vector<float> &b, const std::vector<float> &a);

    float filter(const float x);
    [[nodiscard]] std::string toString() const;

private:
    std::string m_name;
    std::vector<float> m_b;
    std::vector<float> m_a;
    std::deque<float> m_x;
    std::deque<float> m_y;
};

#endif //AC3RF_DECODE_IRRFILTER_H