// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#include <cassert>
#include <sstream>
#include <cmath>
#include "AdaptiveFilter.h"

AdaptiveFilter::AdaptiveFilter(int filter_size, float mu)
    : m_filter_size(filter_size),
      m_mu(mu),
      m_window(filter_size, 0.f),
      m_coeffs(filter_size, 0.f),
      m_filtered(filter_size, 0.f),
      m_sample(0.f)
{
    assert(filter_size >= 0);
    if (filter_size > 0)
        m_coeffs[filter_size / 2] = 1.0f;
}

void AdaptiveFilter::addSample(float sample) {
    if (m_filter_size == 0) {
        m_sample = sample;
        return;
    }
    for (int i = 0; i < m_filter_size - 1; i++)
        m_window[i] = m_window[i + 1];
    m_window[m_filter_size - 1] = sample;

    for (int i = 0; i < m_filter_size - 1; i++)
        m_filtered[i] = m_filtered[i + 1];
    float s = 0.f;
    for (int i = 0; i < m_filter_size; i++)
        s += m_coeffs[i] * m_window[i];
    m_filtered[m_filter_size - 1] = s;
}

void AdaptiveFilter::adaptError(float desired, float actual) {
    if (m_filter_size == 0)
        return;
    // const float adjust = (desired - actual) * m_mu; // LMS
    // const float adjust = -m_mu * actual * (actual * actual - 1.f) * 0.01f; // CMA
    const float adjust = m_mu * (desired > actual ? 1.0f : desired < actual ? -1.0f : 0.0f) * 0.02f;
    for (int i = 0; i < m_filter_size; i++) {
        float leakage = std::fabs(m_coeffs[i]) < 1.f / (1.f + std::fabs(i - (m_filter_size - 1) / 2.0f)) ? 0.f : 1e-5f;
        m_coeffs[i] = m_coeffs[i] - m_coeffs[i] * leakage + m_window[i] * adjust;
    }
}

float AdaptiveFilter::getOutput() const {
    if (m_filter_size == 0)
        return m_sample;
    return m_filtered[m_filter_size - 1];
}

float AdaptiveFilter::calcCenter() const {
    double weighted_sum = 0, sq_sum = 0;
    for (int i = 0; i < m_filter_size; i++) {
        sq_sum += m_coeffs[i] * m_coeffs[i];
        weighted_sum += m_coeffs[i] * m_coeffs[i] * i;
    }
    return sq_sum > 0 ? (float)(weighted_sum / sq_sum) : 0.f;
}

std::string AdaptiveFilter::filterString() const {
    if (m_filter_size == 0)
        return "[non-adaptive]";
    std::ostringstream ss;
    for (int i = 0; i < m_filter_size; i++)
        ss << m_coeffs[i] << " ";
    ss << "center: " << calcCenter();
    return ss.str();
}
