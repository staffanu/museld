// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_EFM_DECODE_EFMDEMODULATOR_H
#define AC3RF_EFM_DECODE_EFMDEMODULATOR_H

#include <atomic>
#include <string>
#include <cstdint>

#include "logging/Logger.h"
#include "../filter/FirFilterStage.h"
#include "../filter/IirFilter.h"
#include "TimingRecovery.h"

class EfmDemodulator {
public:
    EfmDemodulator(Logger &log, double input_sample_frequency, int input_block_size, bool use_simd,
        bool rf_input, int log2_decimation, int adaptive_filter_size, std::optional<std::string> retiming_debug_filename);
    ~EfmDemodulator();

    EfmDemodulator(const EfmDemodulator &) = delete;
    EfmDemodulator &operator=(const EfmDemodulator &) = delete;
    EfmDemodulator(EfmDemodulator &&) = delete;
    EfmDemodulator &operator=(EfmDemodulator &&) = delete;

    // Filters the input samples and performs clock recovery to extract the channel bit stream of 4.3218 M symbols/s
    void demodulate(const float *input_buffer, std::vector<float> &reclocked_data);

    // The largest power-of-two decimation that keeps the decimated sample rate above 8 MHz.
    static int defaultLog2Decimation(double input_sample_frequency);

    // Adaptive FIR filter size in the timing recovery (0 disables the filter).
    // May be called from any thread; the change takes effect at the start of
    // the next demodulate() call, and adaptiveFilterSize() reports the size
    // requested so far.
    void setAdaptiveFilterSize(int size) { m_adaptive_filter_size = size; }
    [[nodiscard]] int adaptiveFilterSize() const { return m_adaptive_filter_size; }

private:
    static IirFilter<5> *makeEllipticLowpassFilter(double Fs);

    Logger &m_log;
    double m_input_sample_frequency;
    int m_input_block_size;
    bool m_rf_input;
    int m_log2_decimation;
    int m_decimation_factor;
    std::vector<FirFilterStage *> m_decimation_filter_stages;
    IirFilter<2> m_remove_dc_filter;
    IirFilter<5> *m_low_pass_filter;
    IirFilter<3> *m_phase_adjust_filter;
    std::vector<float> m_filtered_input;

    TimingRecovery m_timing_recovery;
    std::atomic<int> m_adaptive_filter_size;

    int m_timing_log_period;
    int m_timing_blocks = 0;
    int64_t m_decimation_ns = 0;
    int64_t m_iir_ns = 0;
    int64_t m_reclock_ns = 0;
};

#endif //AC3RF_EFM_DECODE_EFMDEMODULATOR_H
