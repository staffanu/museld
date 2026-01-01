//
// Created by staffanu on 12/30/25.
//

#ifndef AC3RF_DECODE_INTERPOLATINGERASURECONCEALER_H
#define AC3RF_DECODE_INTERPOLATINGERASURECONCEALER_H

#include <vector>
#include "ErasureConcealer.h"

class Logger;

class InterpolatingErasureConcealer : public ErasureConcealer {
public:
    InterpolatingErasureConcealer(Logger &log, std::optional<std::string> debug_filename);

    std::vector<TwoChannelSampleWithErasureFlags> processSamplesImpl(
        const std::vector<TwoChannelSampleWithErasureFlags> &samples_with_erasures) override;
private:
    static constexpr int c_overlap_size = 128;
    std::vector<TwoChannelSampleWithErasureFlags> m_input;
};

#endif //AC3RF_DECODE_INTERPOLATINGERASURECONCEALER_H
