//
// Created by staffanu on 2/18/24.
//

#ifndef MUSECPP_PERCENTILEFILTER_H
#define MUSECPP_PERCENTILEFILTER_H


class PercentileFilter {
public:
    PercentileFilter(float percentile, float initial_estimate);
    PercentileFilter(const PercentileFilter &other) = delete;
    PercentileFilter& operator=(const PercentileFilter &other) = delete;

    void update(float v);
    [[nodiscard]] float getEstimate() const;

private:
    float m_percentile;
    float m_current_estimate;
    int m_counter;
    int m_under_count;
};

#endif //MUSECPP_PERCENTILEFILTER_H
