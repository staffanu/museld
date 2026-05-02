// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_SLOWARERASURECONCEALER_H
#define AC3RF_DECODE_SLOWARERASURECONCEALER_H

#include <deque>
#include <span>
#include <vector>
#include "ErasureConcealer.h"
#include "logging/Logger.h"
#include "../TwoChannelSample.h"

class SlowArErasureConcealer : public ErasureConcealer {
public:
    // The window size is 4 * hop_size, and needs to be at least 8/3 * order, so hos size needs to be at least 2/3 * order.
    SlowArErasureConcealer(Logger &log, int order, int hop_size, std::optional<std::string> debug_filename = std::nullopt);

    SlowArErasureConcealer(const SlowArErasureConcealer &) = delete;
    SlowArErasureConcealer &operator=(const SlowArErasureConcealer &) = delete;
    SlowArErasureConcealer(SlowArErasureConcealer &&) = delete;
    SlowArErasureConcealer &operator=(SlowArErasureConcealer &&) = delete;

    std::vector<TwoChannelSampleWithErasureFlags> processSamplesImpl(const std::vector<TwoChannelSampleWithErasureFlags> &samples_with_erasures) override;

private:
    void processFrame(std::span<TwoChannelSampleWithErasureFlags> samples, std::vector<double> &left_out, std::vector<double> &right_out) const;

    void reconstruct_signal(std::vector<double> const &samples, std::vector<bool> const &burst, std::vector<double> &output) const;
    void update_a(const std::vector<double> &y, std::vector<double> &a) const;
    bool update_x(const std::vector<double> &y, const std::vector<double> &a, const std::vector<int> &t,
                  std::vector<double> &x) const;
    static inline bool solveCholesky(const std::vector<std::vector<double> > &A, std::vector<double> &x);
    static double dotProduct(const std::vector<double> &x, int i_minx, int i_maxx, const std::vector<double> &y, int i_miny, int i_maxy);
    static bool isPresent(int value, const std::vector<int> &x);

    Logger &m_log;
    int m_order;
    int m_hop_size;
    int m_window_size;
    std::vector<double> m_hamming_window;
    std::vector<TwoChannelSampleWithErasureFlags> m_samples;

    // The last four processed frames; newly processed frames are added to the back
    std::deque<std::vector<double>> m_output_buffers[2];
};


#endif //AC3RF_DECODE_SLOWARERASURECONCEALER_H