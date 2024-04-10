//
// Created by staffanu on 2/18/24.
//

#ifndef MUSECPP_PERCENTILEFILTER_H
#define MUSECPP_PERCENTILEFILTER_H

#include "../MuseTypes.h"

class PercentileFilter {
public:
    PercentileFilter(float percentile, float initial_estimate);
    PercentileFilter(const PercentileFilter &other) = delete;
    PercentileFilter& operator=(const PercentileFilter &other) = delete;

    inline void update(float v) {
        if (v < m_current_estimate)
            m_under_count++;
        if (m_counter++ == MUSE_TOTAL_WIDTH * MUSE_TOTAL_HEIGHT) {
            if ((float)m_under_count / (float)(MUSE_TOTAL_WIDTH * MUSE_TOTAL_HEIGHT) > m_percentile)
                m_current_estimate *= 0.99;
            else
                m_current_estimate *= 1.01;
            m_counter = 0;
            m_under_count = 0;
        }
    }

    [[nodiscard]] float getEstimate() const;

private:
    float m_percentile;
    float m_current_estimate;
    int m_counter;
    int m_under_count;
};

#endif //MUSECPP_PERCENTILEFILTER_H
