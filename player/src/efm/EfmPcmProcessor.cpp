// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "EfmPcmProcessor.h"

EfmPcmProcessor::EfmPcmProcessor(Logger &log,
        ErasureConcealer::ConcealmentImplementation impl)
    : m_pop_detector(log),
      m_concealer(ErasureConcealer::create(impl, log, std::nullopt)) {}

EfmPcmProcessor::~EfmPcmProcessor() = default;

std::vector<TwoChannelSample> EfmPcmProcessor::processSamples(
    const std::vector<TwoChannelSampleWithErasureFlags> &raw_samples) {
    return m_concealer->processSamples(m_pop_detector.processSamples(raw_samples));
}

std::string EfmPcmProcessor::statistics() const {
    return m_concealer->erasureStatistics();
}
