// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#ifndef MUSECPP_EFMPCMPROCESSOR_H
#define MUSECPP_EFMPCMPROCESSOR_H

#include <memory>
#include <string>
#include <vector>
#include "PopDetector.h"
#include "TwoChannelSample.h"
#include "concealment/ErasureConcealer.h"

class Logger;

class EfmPcmProcessor {
public:
    explicit EfmPcmProcessor(Logger &log,
        ErasureConcealer::ConcealmentImplementation impl = ErasureConcealer::AutoregressiveModel);
    ~EfmPcmProcessor();
    EfmPcmProcessor(const EfmPcmProcessor &) = delete;
    EfmPcmProcessor &operator=(const EfmPcmProcessor &) = delete;
    EfmPcmProcessor(EfmPcmProcessor &&) = delete;
    EfmPcmProcessor &operator=(EfmPcmProcessor &&) = delete;

    std::vector<TwoChannelSample> processSamples(
        const std::vector<TwoChannelSampleWithErasureFlags> &raw_samples);

    [[nodiscard]] std::string statistics() const;

private:
    PopDetector m_pop_detector;
    std::unique_ptr<ErasureConcealer> m_concealer;
};

#endif //MUSECPP_EFMPCMPROCESSOR_H
