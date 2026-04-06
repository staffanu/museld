// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

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
    void adaptError(float desired, float actual) override;
    [[nodiscard]] float getOutput() const override;
    [[nodiscard]] float calcCenter() const override;
    [[nodiscard]] std::string filterString() const override;

private:
    const int m_filter_size;
    const float m_mu;
    float *m_window; // raw samples -- newest last
    float *m_filter;
    float *m_filtered; // filtered samples -- newest last
};

#endif //AC3RF_DECODE_ADAPTIVEFILTERIMPL_H
