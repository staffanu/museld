// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_GARDNERTIMINGRECOVERY_H
#define AC3RF_DECODE_GARDNERTIMINGRECOVERY_H

#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

#include "logging/Logger.h"
#include "FractionalResampler.h"
#include "AdaptiveFilter.h"

class TimingRecovery {
public:
    TimingRecovery(Logger &log, double input_sample_frequency, int input_block_size,
        int adaptive_filter_size, std::optional<std::string> retiming_debug_filename);

    TimingRecovery(const TimingRecovery &) = delete;
    TimingRecovery &operator=(const TimingRecovery &) = delete;
    TimingRecovery(TimingRecovery &&) = delete;
    TimingRecovery &operator=(TimingRecovery &&) = delete;

    void reclock(const float *input, std::vector<float> &output);

    // Number of taps in the adaptive filter ahead of the timing detector (0
    // bypasses it).  Not safe to call while reclock() runs.
    void setAdaptiveFilterSize(int size) { m_filter.resize(size); }
    [[nodiscard]] int adaptiveFilterSize() const { return m_filter.size(); }

    // Where the bit rate comes from.  ePll: the classic second-order loop,
    // whose integrator accumulates the Mueller-Muller error; under heavy
    // noise that error is biased and the integrator walks to its clamp,
    // after which the clock runs 2 % off for good.  eSync (default): the
    // same loop, but its integrator is held within a window around the bit
    // rate measured from the frame sync pattern, which recurs every 588
    // channel bits whatever the disc looks like.  The loop keeps its full
    // bandwidth for wow, and a biased error can only pull the rate as far
    // as the window allows.
    enum class RateSource { ePll, eSync };
    void setRateSource(RateSource source) { m_rate_source = source; }
    [[nodiscard]] RateSource rateSource() const { return m_rate_source; }

private:
    Logger &m_log;
    const double m_input_sample_frequency;
    const int m_input_block_size;
    FractionalResampler m_resampler;
    AdaptiveFilter m_filter;

    float m_power_estimate;
    uint64_t m_total_symbols;
    uint64_t m_last_adaptive_filter_log;

    static constexpr double c_omega = 2 * M_PI * 1200; // un-dampened frequency
    static constexpr double c_zeta = 0.6; // damping factor
    static constexpr double c_Ts = 1 / 4.3218e6;
    const double c_G1 = 1 - exp(-2 * c_zeta * c_omega * c_Ts);
    const double c_G2 = 1 + exp(-2 * c_omega * c_zeta * c_Ts) - 2 * exp(-c_omega * c_zeta * c_Ts) * cos(c_omega * c_Ts * sqrt(1 - c_zeta * c_zeta));

    const double m_nominal_step_size;
    const double m_GpdGvco;
    const double m_g1; // P
    const double m_g2; // I

    double m_step_size_adjustment;
    double m_error_sum;
    float m_prev_sample;
    double m_avg_speed_deviation;

    RateSource m_rate_source;
    // Sync-derived rate: the state of the sync tracker on the sliced symbols
    static constexpr int c_sync_pattern = 0x801002;     // 24 toggle bits of the frame sync
    static constexpr int c_frame_bits = 588;
    static constexpr int c_sync_window = 4;              // symbols either side of the expected position
    static constexpr int c_sync_max_gap_frames = 8;      // coast this long, then re-acquire
    static constexpr double c_rate_gain = 0.2;           // per accepted sync interval
    static constexpr double c_anchor_window = 0.0015;    // integrator may deviate this much (of the step) from the sync rate
    static constexpr double c_rate_limit = 0.015;        // |rate offset| / nominal step
    bool m_prev_bit;
    uint32_t m_sync_register;
    bool m_sync_locked;
    uint64_t m_last_sync_symbol;
    int m_sync_hits;                                     // consecutive consistent syncs while acquiring
    static constexpr int c_acquire_history = 64;         // recent sync-like patterns kept while acquiring
    static constexpr int c_acquire_frames = 16;          // ... within this many frames
    static constexpr int c_acquire_votes = 3;            // earlier candidates on the same frame phase needed to lock
    uint64_t m_candidates[c_acquire_history];
    int m_candidate_index;
    double m_rate_offset;                                // samples per symbol, from the sync intervals
    double m_input_position;                             // input samples consumed so far (fractional)
    double m_last_sync_position;                         // ... at the last accepted sync
    uint64_t m_sync_intervals, m_sync_losses;

    std::optional<int> m_debug_fd;
};

#endif //AC3RF_DECODE_GARDNERTIMINGRECOVERY_H