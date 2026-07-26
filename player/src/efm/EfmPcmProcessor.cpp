// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cstdint>
#include <algorithm>
#include <cmath>
#include "EfmPcmProcessor.h"

// The first order 50/15 us de-emphasis of IEC 60908 clause 12, bilinear transformed at the
// 44,1 kHz EFM sample rate.  Unity at DC falling to 15/50 (-10,5 dB) at Nyquist, so it can
// only attenuate and cannot overflow the 16 bit samples.
IirFilter<2> EfmPcmProcessor::makeDeemphasisFilter() {
    constexpr double sample_rate = 44100.0;
    constexpr double pole_tau = 50e-6;
    constexpr double zero_tau = 15e-6;

    const double k = 2.0 * sample_rate;
    const double norm = 1.0 + k * pole_tau;
    const std::array<float, 2> b = {
        (float)((1.0 + k * zero_tau) / norm),
        (float)((1.0 - k * zero_tau) / norm)
    };
    const std::array<float, 2> a = {1.f, (float)((1.0 - k * pole_tau) / norm)};

    return IirFilter<2>("EFM de-emphasis", b, a);
}

EfmPcmProcessor::EfmPcmProcessor(Logger &log,
        ErasureConcealer::ConcealmentImplementation impl)
    : m_pop_detector(log),
      m_concealer(ErasureConcealer::create(impl, log, std::nullopt)),
      m_deemphasis{{makeDeemphasisFilter(), makeDeemphasisFilter()}} {}

EfmPcmProcessor::~EfmPcmProcessor() = default;

std::vector<TwoChannelSample> EfmPcmProcessor::processSamples(
    const std::vector<TwoChannelSampleWithErasureFlags> &raw_samples, bool pre_emphasis) {
    auto samples = m_concealer->processSamples(m_pop_detector.processSamples(raw_samples));

    // The filter state goes stale while pre-emphasis is off, but the flag only changes during a
    // pause of at least 2 s, so whatever follows a switch is silence anyway.
    if (pre_emphasis)
        for (auto &sample: samples)
            for (int channel = 0; channel < 2; channel++)
                sample.samples[channel] = (int16_t)std::lround(std::clamp(
                    m_deemphasis[channel].filter((float)sample.samples[channel]), -32768.f, 32767.f));

    return samples;
}

std::string EfmPcmProcessor::statistics() const {
    return m_concealer->erasureStatistics();
}
