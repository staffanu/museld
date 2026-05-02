// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_LINEARINTERPOLATIONERASURECONCEALER_H
#define AC3RF_DECODE_LINEARINTERPOLATIONERASURECONCEALER_H

#include <vector>
#include "ErasureConcealer.h"

class Logger;

class LinearInterpolationErasureConcealer : public ErasureConcealer {
public:
    LinearInterpolationErasureConcealer(Logger &log, int max_interpolated_erasures, std::optional<std::string> debug_filename);

protected:
    friend class ArErasureConcealer;

    std::vector<TwoChannelSampleWithErasureFlags> processSamplesImpl(
        const std::vector<TwoChannelSampleWithErasureFlags> &samples_with_erasures) override;
private:
    static constexpr int c_overlap_size = 128;
    std::vector<TwoChannelSampleWithErasureFlags> m_input;
    int m_max_interpolated_erasures;
};

#endif //AC3RF_DECODE_LINEARINTERPOLATIONERASURECONCEALER_H
