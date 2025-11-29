//
// Created by Staffan Ulfberg on 11/25/25.
//

#include <cassert>
#include <format>
#include <sstream>
#include "IirFilter.h"

IirFilter::IirFilter(const std::string name, const std::vector<float> &b, const std::vector<float> &a)
    : m_name(name), m_b(b), m_a(a) {
    assert(a[0] == 1.f);
    m_x.resize(b.size());
    m_y.resize(a.size());
}

float IirFilter::filter(const float x) {
    m_x.push_front(x);
    m_x.pop_back();

    float y = 0;
    for (int i = 0; i < m_b.size(); i++)
        y += m_b[i] * m_x[i];
    for (int i = 1; i < m_a.size(); i++)
        y -= m_a[i] * m_y[i - 1];

    m_y.push_front(y);
    m_y.pop_back();
    return y;
}

[[nodiscard]] std::string IirFilter::toString() const {
    std::ostringstream ss;
    ss << std::format("{}: size: {}/{}, b:", m_name, m_b.size(), m_a.size());
    for (int i = 0; i < m_b.size(); i++) // we store the coefficients reversed
        ss << " " << m_b[i];;
    ss << ", a:";
    for (int i = 0; i < m_a.size(); i++) // we store the coefficients reversed
        ss << " " << m_a[i];;
    return ss.str();
}