//
// Created by staffanu on 11/15/25.
//

#ifndef AC3RF_DECODE_ADAPTIVEFILTERIMPL_H
#define AC3RF_DECODE_ADAPTIVEFILTERIMPL_H

#include <string>
#include "AdaptiveFilter.h"

class AdaptiveFilterImpl : public AdaptiveFilter {
public:
    explicit AdaptiveFilterImpl(int filter_size, float mu);

    AdaptiveFilterImpl(const AdaptiveFilterImpl &) = delete;
    AdaptiveFilterImpl &operator=(const AdaptiveFilterImpl &) = delete;
    AdaptiveFilterImpl(AdaptiveFilterImpl &&) = delete;
    AdaptiveFilterImpl &operator=(AdaptiveFilterImpl &&) = delete;

    ~AdaptiveFilterImpl() override;

    [[nodiscard]] int size() const override;
    void addSample(float sample) override;
    void adaptError(float e) override;
    void getLast3(float &f1, float&f2, float &f3) override;
    std::string filterString() override;

private:
    const int m_filter_size;
    const float m_mu;
    float *m_window; // raw samples -- newest last
    float *m_filter;
    float *m_filtered; // filtered samples -- newest last
};

#endif //AC3RF_DECODE_ADAPTIVEFILTERIMPL_H
