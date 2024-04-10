//
// Created by staffanu on 2/18/24.
//

#include "PercentileFilter.h"

PercentileFilter::PercentileFilter(float percentile, float initial_estimate)
        : m_percentile(percentile),
          m_current_estimate(initial_estimate),
          m_counter(0),
          m_under_count(0) {
}

float PercentileFilter::getEstimate() const {
    return m_current_estimate;
};
