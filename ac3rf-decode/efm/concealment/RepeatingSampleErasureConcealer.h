//
// Created by staffanu on 12/30/25.
//

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
