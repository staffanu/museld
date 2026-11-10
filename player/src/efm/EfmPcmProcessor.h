// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_EFMPCMPROCESSOR_H
#define MUSECPP_EFMPCMPROCESSOR_H

#include <array>
#include <memory>
#include <string>
#include <vector>
#include "PopDetector.h"
#include "TwoChannelSample.h"
#include "concealment/ErasureConcealer.h"
#include "../filter/IirFilter.h"

class Logger;

class EfmPcmProcessor {
public:
    // concealment_debug_filename: the concealer writes six int16 per sample
    // (input L/R, concealed L/R, erased L/R) to it
    explicit EfmPcmProcessor(Logger &log,
        ErasureConcealer::ConcealmentImplementation impl = ErasureConcealer::AutoregressiveModel,
        std::optional<std::string> concealment_debug_filename = std::nullopt);
    ~EfmPcmProcessor();
    EfmPcmProcessor(const EfmPcmProcessor &) = delete;
    EfmPcmProcessor &operator=(const EfmPcmProcessor &) = delete;
    EfmPcmProcessor(EfmPcmProcessor &&) = delete;
    EfmPcmProcessor &operator=(EfmPcmProcessor &&) = delete;

    std::vector<TwoChannelSample> processSamples(
        const std::vector<TwoChannelSampleWithErasureFlags> &raw_samples, bool pre_emphasis);

    [[nodiscard]] std::string statistics() const;

private:
    static IirFilter<2> makeDeemphasisFilter();

    PopDetector m_pop_detector;
    std::unique_ptr<ErasureConcealer> m_concealer;
    std::array<IirFilter<2>, 2> m_deemphasis;
};

#endif //MUSECPP_EFMPCMPROCESSOR_H
