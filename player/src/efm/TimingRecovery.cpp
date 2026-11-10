// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <sys/fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <format>
#include <stdexcept>

#ifndef O_BINARY
#  define O_BINARY 0
#endif

#include "TimingRecovery.h"


TimingRecovery::TimingRecovery(Logger &log, double input_sample_frequency, int input_block_size,
                               int adaptive_filter_size, std::optional<std::string> retiming_debug_filename)
  : m_log(log),
    m_input_sample_frequency(input_sample_frequency),
    m_input_block_size(input_block_size),
    m_resampler(input_block_size),
    m_filter(adaptive_filter_size, 1e-3f),
    m_power_estimate(1),
    m_total_symbols(0),
    m_last_adaptive_filter_log(0),
    m_nominal_step_size(input_sample_frequency / 4.3218e6),
    m_GpdGvco(0.5 / m_nominal_step_size),
    m_g1(c_G1 / m_GpdGvco),
    m_g2(c_G2 / m_GpdGvco),
    m_step_size_adjustment(0),
    m_error_sum(0),
    m_prev_sample(0),
    m_avg_speed_deviation(0),
    m_rate_source(RateSource::eSync),
    m_prev_bit(false),
    m_sync_register(0),
    m_sync_locked(false),
    m_last_sync_symbol(0),
    m_sync_hits(0),
    m_candidates{},
    m_candidate_index(0),
    m_rate_offset(0),
    m_input_position(0),
    m_last_sync_position(0),
    m_sync_intervals(0),
    m_sync_losses(0),
    m_debug_fd(std::nullopt) {

    if (retiming_debug_filename.has_value()) {
        m_debug_fd = open(retiming_debug_filename->c_str(), O_WRONLY | O_TRUNC | O_CREAT | O_BINARY, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (m_debug_fd == -1)
            throw std::runtime_error(std::format("Error writing to output: {}", strerror(errno)));
    }
}


void TimingRecovery::reclock(const float *input, std::vector<float> &output) {

    m_resampler.updateInput(input);

    output.clear();
    float s;
    while (m_resampler.advanceTimeAndResample(m_nominal_step_size + m_step_size_adjustment, s)) {
        m_total_symbols++;
        m_input_position += m_nominal_step_size + m_step_size_adjustment;
        float power_alpha = m_total_symbols < 5000 ? 0.1f : 0.0001f;

        m_power_estimate = (1 - power_alpha) * m_power_estimate + power_alpha * s * s;
        float gain = sqrt(1 / (m_power_estimate + 1.0));
        m_filter.addSample(s * gain);

        // Initially gain is uncertain and also the signal is not DC
        if (m_total_symbols < 5000)
            continue;

        float sample = m_filter.getOutput();

        // error positive: we're sampling too early, so increase sample spacing
        // Mueller and Muller timing detector
        double error = (m_prev_sample > 0 ? 1. : -1.) * sample - (sample > 0 ? 1. : -1.) * m_prev_sample;
        //printf("%f %f %f  error=%f sum=%f\n", sample_before, sample, sample_after, error, m_error_sum);
        error = std::max(std::min(error, m_nominal_step_size / m_g1 * 0.02), -m_nominal_step_size / m_g1 * 0.02);

        // Sync tracking on the sliced symbols: the frame sync's 24 toggle bits
        // recur every 588 symbols.  Once two syncs a whole number of frames
        // apart have been seen, the symbol count between syncs measures the
        // rate error directly and without bias: emitting more than 588
        // symbols per frame means the step is too small.
        const bool bit = sample > 0;
        m_sync_register = ((m_sync_register << 1) | (bit != m_prev_bit ? 1u : 0u)) & 0xffffffu;
        m_prev_bit = bit;
        const int distance = __builtin_popcount(m_sync_register ^ (uint32_t)c_sync_pattern);
        if (m_rate_source == RateSource::eSync) {
            const uint64_t n = m_total_symbols;
            if (!m_sync_locked) {
                // Acquire by voting: sync-like patterns (up to two bits wrong)
                // whose positions agree modulo the frame length, within the
                // last few frames, mark the frame phase even when most syncs
                // are damaged
                if (distance <= 2) {
                    int votes = 0;
                    for (int i = 0; i < c_acquire_history; i++) {
                        const uint64_t c = m_candidates[i];
                        if (c == 0 || n - c > (uint64_t)c_acquire_frames * c_frame_bits) continue;
                        const int64_t off = (int64_t)((n - c) % c_frame_bits);
                        if (off <= 2 || off >= c_frame_bits - 2) votes++;
                    }
                    m_candidates[m_candidate_index] = n;
                    m_candidate_index = (m_candidate_index + 1) % c_acquire_history;
                    if (votes >= c_acquire_votes) {
                        m_sync_locked = true;
                        m_last_sync_symbol = n;
                        m_last_sync_position = m_input_position;
                        for (auto &c : m_candidates) c = 0;
                        m_log.debug(eAudio, std::format("EFM sync rate: locked at symbol {}", n));
                    }
                }
            } else {
                const uint64_t since = n - m_last_sync_symbol;
                const int k = (int)((since + c_frame_bits / 2) / c_frame_bits);
                const int64_t off = (int64_t)since - (int64_t)k * c_frame_bits;
                if (k >= 1 && distance <= 2 && off >= -c_sync_window && off <= c_sync_window) {
                    // A sync where one is expected: the input samples consumed
                    // since the previous one span exactly k frames of 588 bits,
                    // whatever the phase loop did meanwhile, so they measure
                    // the bit period directly
                    const double measured = (m_input_position - m_last_sync_position) / (k * c_frame_bits);
                    m_rate_offset += c_rate_gain * (measured - m_nominal_step_size - m_rate_offset);
                    m_rate_offset = std::max(std::min(m_rate_offset, c_rate_limit * m_nominal_step_size),
                                             -c_rate_limit * m_nominal_step_size);
                    m_last_sync_symbol = n;
                    m_last_sync_position = m_input_position;
                    m_sync_intervals++;
                } else if (since > (uint64_t)c_sync_max_gap_frames * c_frame_bits + c_sync_window) {
                    m_sync_locked = false;
                    m_sync_hits = 0;
                    m_sync_losses++;
                    m_log.debug(eAudio, std::format("EFM sync rate: lost at symbol {} (rate offset {:.0f} ppm)", n,
                                                    1e6 * m_rate_offset / m_nominal_step_size));
                }
            }
        }
        // The loop as before; with the sync rate available, its integrator is
        // anchored to it
        m_error_sum += error;
        double lo = -m_nominal_step_size / m_g2 * 0.02, hi = m_nominal_step_size / m_g2 * 0.02;
        if (m_rate_source == RateSource::eSync && m_sync_intervals > 0) {
            lo = (m_rate_offset - c_anchor_window * m_nominal_step_size) / m_g2;
            hi = (m_rate_offset + c_anchor_window * m_nominal_step_size) / m_g2;
        }
        m_error_sum = std::max(std::min(m_error_sum, hi), lo);
        m_step_size_adjustment = error * m_g1 + m_error_sum * m_g2;
        if (m_rate_source == RateSource::eSync && m_sync_intervals == 0)
            m_rate_offset = m_error_sum * m_g2; // the integrator's rate seeds the sync tracker
        assert(!std::isnan(m_step_size_adjustment) && abs(m_step_size_adjustment) <= m_nominal_step_size * 0.04);

        m_filter.adaptError(sample > 0 ? 1.f : -1.f, sample);

        if (m_debug_fd.has_value()) {
            float debug_floats[3] = { sample, (float)error, (float)(m_step_size_adjustment / m_nominal_step_size) };
            int r = write(m_debug_fd.value(), debug_floats, 3 * sizeof(float));
            if (r == -1)
                throw std::runtime_error(std::format("Error writing to debug output file: {}", strerror(errno)));
        }

        output.emplace_back(sample);
        m_prev_sample = sample;
    }

    m_avg_speed_deviation = (1 - 1e-5) * m_avg_speed_deviation + 1e-5 * m_step_size_adjustment / m_nominal_step_size;
    if (m_total_symbols - m_last_adaptive_filter_log > 4321800) {
        if (m_filter.size() != 0)
            m_log.debug(eAudio,
                std::format("Adaptive filter coefficients: {}.  DPLL adjustment: {:.0f} ppm",
                m_filter.filterString(), 1e6 * m_avg_speed_deviation));
        else
            m_log.debug(eAudio,
                std::format("DPLL adjustment: {:.0f} ppm",
                1e6 * m_avg_speed_deviation));
        if (m_rate_source == RateSource::eSync)
            m_log.debug(eAudio, std::format("EFM sync rate: {}, {} intervals, {} losses, rate offset {:.0f} ppm",
                                            m_sync_locked ? "locked" : "acquiring", m_sync_intervals, m_sync_losses,
                                            1e6 * m_rate_offset / m_nominal_step_size));

        m_last_adaptive_filter_log = m_total_symbols;
    }
}
