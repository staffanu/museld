// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_PERCENTILEFILTER_H
#define MUSECPP_PERCENTILEFILTER_H

class PercentileFilter {
public:
    PercentileFilter(int update_threshold, float percentile, float initial_estimate);
    PercentileFilter(const PercentileFilter &other) = delete;
    PercentileFilter& operator=(const PercentileFilter &other) = delete;

    inline void update(float v) {
        if (v < m_current_estimate)
            m_under_count++;
        if (m_counter++ == c_update_threshold) {
            if ((float)m_under_count / (float)(c_update_threshold) > c_percentile)
                m_current_estimate *= 0.99;
            else
                m_current_estimate *= 1.01;
            m_counter = 0;
            m_under_count = 0;
        }
    }

    [[nodiscard]] float getEstimate() const;

private:
    const int c_update_threshold;
    const float c_percentile;
    float m_current_estimate;
    int m_counter;
    int m_under_count;
};

#endif //MUSECPP_PERCENTILEFILTER_H
