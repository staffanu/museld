// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "PercentileFilter.h"

PercentileFilter::PercentileFilter(int update_threshold, float percentile, float initial_estimate)
        : c_update_threshold(update_threshold),
          c_percentile(percentile),
          m_current_estimate(initial_estimate),
          m_counter(0),
          m_under_count(0) {
}

float PercentileFilter::getEstimate() const {
    return m_current_estimate;
}
