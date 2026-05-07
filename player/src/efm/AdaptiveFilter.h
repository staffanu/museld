// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_ADAPTIVEFILTER_H
#define AC3RF_DECODE_ADAPTIVEFILTER_H

#include <string>
#include <vector>

// LMS adaptive FIR filter with sign-error update.
// filter_size == 0: non-adaptive passthrough (addSample stores, getOutput returns, adaptError is a no-op).
// filter_size >  0: adaptive FIR with the given number of taps.
class AdaptiveFilter {
public:
    explicit AdaptiveFilter(int filter_size, float mu = 1e-3f);

    AdaptiveFilter(const AdaptiveFilter &) = delete;
    AdaptiveFilter &operator=(const AdaptiveFilter &) = delete;
    AdaptiveFilter(AdaptiveFilter &&) = delete;
    AdaptiveFilter &operator=(AdaptiveFilter &&) = delete;

    void addSample(float sample);
    void adaptError(float desired, float actual);
    [[nodiscard]] float getOutput() const;
    [[nodiscard]] int size() const { return m_filter_size; }
    [[nodiscard]] float calcCenter() const;
    [[nodiscard]] std::string filterString() const;

private:
    const int m_filter_size;
    const float m_mu;
    std::vector<float> m_window;
    std::vector<float> m_coeffs;
    std::vector<float> m_filtered;
    float m_sample = 0.f;
};

#endif //AC3RF_DECODE_ADAPTIVEFILTER_H
