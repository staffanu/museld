// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_IRRFILTER_H
#define AC3RF_DECODE_IRRFILTER_H

#include <array>
#include <string>
#include <cassert>
#include <format>
#include <sstream>

// IIR filter implementation that requires b and a to have the same size.
// This happens to be the case for all the filters in the EFM demodulator,
// and exploiting this fact makes the loops a tiny bit faster.

template<int N>
class IirFilter {
public:
    IirFilter(const std::string name, const std::array<float, N> &b, const std::array<float, N> &a)
    : m_name(name), m_b(b), m_a(a), m_x{}, m_y{}, m_x0_ix(0) {
        static_assert(N <= c_max_filter_size);
    }

    float filter(const float x) {
        m_x0_ix = (m_x0_ix + 1) & (c_max_filter_size - 1);
        m_x[m_x0_ix] = x;

        int ix = m_x0_ix;
        float y = m_b[0] * m_x[ix];
        for (int i = 1; i < N; i++) {
            ix = (ix - 1) & (c_max_filter_size - 1);
            y += m_b[i] * m_x[ix];
            y -= m_a[i] * m_y[ix];
        }

        m_y[m_x0_ix] = y / m_a[0];

        return y;
    }

    [[nodiscard]] std::string toString() const {
        std::ostringstream ss;
        ss << std::format("{}: size: {}/{}, b:", m_name, m_b.size(), m_a.size());
        for (int i = 0; i < m_b.size(); i++) // we store the coefficients reversed
            ss << " " << m_b[i];;
        ss << ", a:";
        for (int i = 0; i < m_a.size(); i++) // we store the coefficients reversed
            ss << " " << m_a[i];;
        return ss.str();
    }

private:
    static constexpr int c_max_filter_size = 16;

    std::string m_name;
    std::array<float, N> m_b;
    std::array<float, N> m_a;

    std::array<float, c_max_filter_size> m_x;
    std::array<float, c_max_filter_size> m_y;
    int m_x0_ix; // index of x[0] in the vector (also index of y[0] between calls to filter)
};

#endif //AC3RF_DECODE_IRRFILTER_H
