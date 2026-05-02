// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_ARERASURECONCEALER_H
#define AC3RF_DECODE_ARERASURECONCEALER_H

#include <deque>

#include "ErasureConcealer.h"
#include "LinearInterpolationErasureConcealer.h"
#include "logging/Logger.h"


class ArErasureConcealer : public ErasureConcealer {
public:
    // The window size is 4 * hop_size, and needs to be at least 8/3 * order, so hos size needs to be at least 2/3 * order.
    ArErasureConcealer(Logger &log, int order, double mu, int max_li_erasures, int crossfade_length, std::optional<std::string> debug_filename);

    ArErasureConcealer(const ArErasureConcealer &) = delete;
    ArErasureConcealer &operator=(const ArErasureConcealer &) = delete;
    ArErasureConcealer(ArErasureConcealer &&) = delete;
    ArErasureConcealer &operator=(ArErasureConcealer &&) = delete;

    std::vector<TwoChannelSampleWithErasureFlags> processSamplesImpl(const std::vector<TwoChannelSampleWithErasureFlags> &samples_with_erasures) override;

private:

    Logger &m_log;
    LinearInterpolationErasureConcealer m_linear_interpolator;
    const int m_order;
    const double m_mu;
    const int m_error_replay_size;
    const int m_crossfade_length;

    // input history is used to update the AR model
    std::deque<TwoChannelSampleWithErasureFlags> m_input_history; // newest in front
    // output history is used to predict the next sample when data is missing
    // notice that corrected/changed data is used to predict the next sample, but not to train the model
    std::deque<TwoChannelSampleWithErasureFlags> m_output_history; // newest in front
    std::vector<double> m_a[2]; // for left and right channels

    // residual errors
    std::deque<double> m_error_history[2];

    uint64_t m_index;
    uint64_t m_last_valid_index[2];
    uint64_t m_last_erased_index[2];
};


#endif //AC3RF_DECODE_ARERASURECONCEALER_H