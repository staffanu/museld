//
// Created by staffanu on 11/15/25.
//

#include <cassert>
#include <sstream>
#include "AdaptiveFilterImpl.h"

AdaptiveFilterImpl::AdaptiveFilterImpl(int filter_size, float mu)
: m_filter_size(filter_size),
  m_mu(mu),
  m_window{},
  m_filter{},
  m_filtered{}
{
    assert(filter_size >= 3);
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

void AdaptiveFilterImpl::add_sample(float sample) {
    for (int i = 0; i < m_filter_size - 1; i++)
        m_window[i] = m_window[i + 1];
    m_window[m_filter_size - 1] = sample;

    for (int i = 0; i < m_filter_size - 1; i++)
        m_filtered[i] = m_filtered[i + 1];
    float s = 0.f;
    for (int i = 0; i < m_filter_size; i++)
        s += m_filter[i] * m_window[i];
    m_filtered[m_filter_size - 1] = s;

    // printf("Added %f, last filtered now %f\n", sample, s);
}

void AdaptiveFilterImpl::adaptError(float e) {
    for (int i = 0; i < m_filter_size; i++)
        m_filter[i] += m_window[i] * e * m_mu;
}

void AdaptiveFilterImpl::getLast3(float &f1, float&f2, float &f3) {
    f1 = m_filtered[m_filter_size - 3];
    f2 = m_filtered[m_filter_size - 2];
    f3 = m_filtered[m_filter_size - 1];
}

std::string AdaptiveFilterImpl::filterString() {
    std::ostringstream ss;
    for (int i = 0; i < m_filter_size; i++)
        ss << m_filter[i] << " ";
    return ss.str();
}
