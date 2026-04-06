// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_REPEATINGSAMPLEERASURECONCEALER_H
#define AC3RF_DECODE_REPEATINGSAMPLEERASURECONCEALER_H

#include <vector>
#include "ErasureConcealer.h"

class Logger;

class RepeatingSampleErasureConcealer : public ErasureConcealer {
public:
    RepeatingSampleErasureConcealer(Logger &log, std::optional<std::string> debug_filename);

    std::vector<TwoChannelSampleWithErasureFlags> processSamplesImpl(
        const std::vector<TwoChannelSampleWithErasureFlags> &samples_with_erasures) override;
private:
    TwoChannelSampleWithErasureFlags m_prev_sample;
};

#endif //AC3RF_DECODE_REPEATINGSAMPLEERASURECONCEALER_H
