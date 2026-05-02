// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#include <cassert>
#include <sstream>
#include <cmath>
#include "AdaptiveFilterImpl.h"

AdaptiveFilterImpl::AdaptiveFilterImpl(int filter_size, float mu)
: m_filter_size(filter_size),
  m_mu(mu),
  m_window{},
  m_filter{},
  m_filtered{}
{
    assert(filter_size >= 1);
    m_window = new float[filter_size];
    m_filter = new float[filter_size];
    m_filtered = new float[filter_size];

    for (int i = 0; i < filter_size; i++)
        m_filter[i] = 0.f;
    m_filter[filter_size / 2] = 1.0;
}

AdaptiveFilterImpl::~AdaptiveFilterImpl() {
    delete[] m_window;
    delete[] m_filter;
    delete[] m_filtered;
}

int AdaptiveFilterImpl::size() const {
    return m_filter_size;
}

void AdaptiveFilterImpl::addSample(float sample) {
    for (int i = 0; i < m_filter_size - 1; i++)
        m_window[i] = m_window[i + 1];
    m_window[m_filter_size - 1] = sample;

    for (int i = 0; i < m_filter_size - 1; i++)
        m_filtered[i] = m_filtered[i + 1];
    float s = 0.f;
    for (int i = 0; i < m_filter_size; i++)
        s += m_filter[i] * m_window[i];
    m_filtered[m_filter_size - 1] = s;
}

void AdaptiveFilterImpl::adaptError(float desired, float actual) {
    // const float adjust = (desired - actual) * m_mu; // LMS
    // const float adjust = -m_mu * actual * (actual * actual - 1.f) * 0.01f; // CMA
    const float adjust = m_mu * (desired > actual ? 1.0f : desired < actual ? -1.0f : 0.0f) * 0.02f; // LMS using error sign only

    for (int i = 0; i < m_filter_size; i++) {
        float leakage = std::fabs(m_filter[i]) < 1.f / (1.f + fabs(i - (m_filter_size - 1) / 2.0)) ? 0.f : 1e-5;
        m_filter[i] = m_filter[i] - m_filter[i] * leakage + m_window[i] * adjust;
    }
}

float AdaptiveFilterImpl::getOutput() const {
    return m_filtered[m_filter_size - 1];
}

float AdaptiveFilterImpl::calcCenter() const {
    double weighted_sum = 0;
    double sq_sum = 0;
    for (int i = 0; i < m_filter_size; i++) {
        sq_sum += m_filter[i] * m_filter[i];
        weighted_sum += m_filter[i] * m_filter[i] * i;
    }
    return weighted_sum / sq_sum;
}

std::string AdaptiveFilterImpl::filterString() const {
    std::ostringstream ss;
    for (int i = 0; i < m_filter_size; i++)
        ss << m_filter[i] << " ";
    ss << "center: " << calcCenter();
    return ss.str();
}
