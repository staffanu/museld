// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <cassert>
#include <filesystem>
#include "musevk/VulkanBuffer.h"
#include "NtscFrameReader.h"

#include "NtscRfDemodulator.h"
#include "filter/WindowedSinc.h"
#include "logging/Logger.h"
#include "util/Interpolate.h"

using namespace std;

namespace {
    // Pulse widths on the lowpassed signal, in microseconds
    constexpr double c_hsync_width_min = 3.0, c_hsync_width_max = 6.5;
    constexpr double c_broad_width_min = 20.0, c_broad_width_max = 32.0;
    // Hysteresis slicer levels (the demodulated video is normalized: sync tip
    // ~0.0, blanking ~0.3)
    constexpr float c_slice_low = 0.12f, c_slice_high = 0.18f;
    constexpr float c_blank_nominal = 0.30f;
    // Kalman noise model, expressed in seconds so it is capture-rate
    // independent.  The sync timestamp noise is only the initial value: the
    // running estimate from the data takes over (floored), because the
    // innovation-based period-noise adaptation is only sound when R is
    // right -- with R asserted too large the normalized innovations read
    // small and the adaptation stiffens the filter until it can no longer
    // follow the wow.  The per-line period process noise is what the
    // adaptation scales; its floor keeps the filter able to follow the
    // measured period slew (up to ~2 ns per line on the white squall disc)
    // whatever the adaptation does.
    constexpr double c_sigma_meas_s = 42e-9;
    constexpr double c_sigma_meas_floor_s = 8e-9;
    constexpr double c_sigma_qp_s = 4.8e-9;
    constexpr double c_sigma_qt_s = 1.6e-9;
    constexpr double c_qscale_min = 0.5, c_qscale_max = 30.0;
    constexpr double c_kal_gate = 5.0; // innovation gate, in predicted sigmas
    // Two pulses corroborate each other when they lie a whole number of lines
    // apart to this tolerance per line of separation (wow moves a line by a
    // few tens of ns; junk sits microseconds off)
    constexpr double c_corroborate_s = 0.5e-6;
}

NtscFrameReader::NtscFrameReader(
        Logger &log, const std::string &executable_dir, musevk::VulkanManager &vulkan_manager,
        const std::string &filename, InputFormat input_format, double sample_rate,
        double initial_seek_seconds, bool benchmark_shaders, AudioTrack audio_track,
        const std::optional<std::string> &output_filename)
        : FrameReader(log, filename,
                      filesystem::is_fifo(filename),
                      initial_seek_seconds, output_filename),
          m_demodulator(nullptr),
          m_sample_rate(sample_rate / NtscRfDemodulatorConstants::c_video_decimation_rate),
          m_input_samples_decimation_rate(NtscRfDemodulatorConstants::c_video_decimation_rate),
          m_p_nominal(0),
          m_input_buffer(nullptr),
          m_input_dropout_buffer(nullptr),
          m_sub_buffer_input_offsets{},
          m_blocks_fetched(0),
          m_stream_pos(0),
          m_sync_boxcar_phase(0),
          m_sync_dec_count(0),
          m_sync_filt{},
          m_sync_delay(0),
          m_sync_below(false),
          m_sync_fall_idx(0),
          m_sync_rise_idx(0),
          m_blank_level(c_blank_nominal),
          m_lattice_valid(false),
          m_lat_t(0),
          m_p_run(0),
          m_last_meas_k(0),
          m_slip_shift(0),
          m_slip_first_k(0),
          m_slip_count(0),
          m_last_dt(0),
          m_last_dk(0),
          m_d2_mean(0.7979 * sqrt(6.0) * c_sigma_meas_s * m_sample_rate),
          m_kal_valid(false),
          m_kal{},
          m_kal_head{},
          m_kal_head_k(0),
          m_k_final(0),
          m_qscale(1.0),
          m_nis_avg(1.0),
          m_adapted_to_k(-1),
          m_frame_resid_bad(0),
          m_frame_meas(0),
          m_frame_log_k(0),
          m_curve_base(0),
          m_anchored(false),
          m_timebase_restarted(false),
          m_line1_k(0),
          m_pending_drift(0),
          m_frame_start_offset(0),
          m_frame_period(0),
          m_process_elapsed_ms(0),
          m_read_input_elapsed_ms(0),
          m_timed_frames(0) {
    m_demodulator = new NtscRfDemodulator(log, executable_dir, m_filename, sample_rate, vulkan_manager,
                                          input_format, benchmark_shaders, audio_track);
}

bool NtscFrameReader::initialize(std::vector<std::unique_ptr<NtscInputBlock>> &buffers) {
    m_demodulator->initialize(m_demodulator->numberOfBlockBuffers());

    m_p_nominal = m_sample_rate * NtscInputBlock::c_samples_per_video_line
                  / NtscInputBlock::c_video_sampling_frequency;

    // The sync pass lowpass removes chroma, burst and crosstalk from the
    // sync path entirely.  It is a full-rate filter -- decimating first
    // would fold the chroma into the sync band (at 62.5 MHz captures the
    // subcarrier lands 0.33 MHz from the fold) -- but evaluated only at the
    // decimated instants, so the cost is ntaps/decim multiplies per sample.
    m_sync_fir = WindowedSinc::low_pass<float>(49, m_sample_rate, 0.4e6);
    m_sync_fir_in.assign(64, 0.f);
    m_sync_delay = (m_sync_fir.size() - 1) / 2.0;

    m_input_buffer = (float *)calloc(c_input_buffer_size, sizeof(float));
    m_input_dropout_buffer = (uint8_t *)calloc(c_input_buffer_size, 1);
    assert(m_input_buffer != nullptr && m_input_dropout_buffer != nullptr);

    return FrameReader::initialize(buffers);
}

// Idempotent: runs both from the explicit teardown path and from the destructor.
void NtscFrameReader::cleanup() {
    // Stop the demodulator before FrameReader::cleanup() joins the reader
    // thread: that thread may be waiting inside getNextDemodulatedBlock(),
    // whose predicate tests the demodulator's stop flag, not ours.
    if (m_demodulator != nullptr)
        m_demodulator->requestStop();

    FrameReader::cleanup();

    free(m_input_buffer);
    m_input_buffer = nullptr;
    free(m_input_dropout_buffer);
    m_input_dropout_buffer = nullptr;

    if (m_demodulator != nullptr) {
        m_demodulator->cleanup();
        delete m_demodulator;
        m_demodulator = nullptr;
    }
}

void NtscFrameReader::seek(double seconds) {
    if (!m_input_is_realtime) {
        m_demodulator->seek(seconds);
        resetTimebase("seek");
    }
}

void NtscFrameReader::setAudioTrack(AudioTrack track) {
    // The audio tracks are alternatives: only the selected one is demodulated
    if (m_demodulator != nullptr)
        m_demodulator->setAudioTrack(track);
}

void NtscFrameReader::setAnalogCx(bool enabled) {
    if (m_demodulator != nullptr)
        m_demodulator->setAnalogCx(enabled);
}

void NtscFrameReader::resetTimebase(const char *why) {
    m_lattice_valid = false;
    m_kal_valid = false;
    m_kal_head_k = 0;
    m_meas.clear();
    m_broad_falls.clear();
    m_curve.clear();
    m_anchored = false;
    m_pending_drift = 0;
    m_qscale = 1.0;
    m_nis_avg = 1.0;
    m_adapted_to_k = -1;
    m_k_final = 0;
    m_last_meas_k = 0;
    m_slip_count = 0;
    m_last_dt = 0;
    m_last_dk = 0;
    m_d2_mean = 0.7979 * sqrt(6.0) * c_sigma_meas_s * m_sample_rate;
    m_frame_resid_bad = 0;
    m_frame_meas = 0;
    m_frame_log_k = 0;
    m_log.info(eInput, std::format("NtscFrameReader: timebase reset ({})", why));
}

void NtscFrameReader::threadFunc() {
    unique_ptr<NtscInputBlock> output_block = nullptr;

    for (;;) {
        if (output_block == nullptr) {
            unique_lock<std::mutex> lock(m_mutex);
            if (m_input_is_realtime && m_vacant_input_buffers.empty()) {
                // discard a filled output_block -- this is better than having the writer to the fifo wait
                m_log.warn(eInput, "Discarding filled block due to overrun");
                assert(!m_filled_input_buffers.empty());
                m_vacant_input_buffers.push_back(std::move(m_filled_input_buffers.back()));
                m_filled_input_buffers.pop_back();
            }
            m_cv_vacant.wait(lock, [this]{return m_stop_request || !m_vacant_input_buffers.empty();});
            if (m_stop_request) {
                m_log.info(eInput, "NtscFrameReader: stop requested");
                break;
            }
            output_block = std::move(m_vacant_input_buffers.front());
            m_vacant_input_buffers.pop_front();
            output_block->efm_data.clear();
            output_block->analog_data.clear();
            output_block->ac3_frames.clear();
        }

        if (!process(output_block)) {
            m_log.info(eInput, m_stop_request ? "NtscFrameReader: stop requested"
                                              : "NtscFrameReader: end of file");
            break;
        }

        output_block->input_offset = m_frame_start_offset;
        output_block->timebase_restarted = m_timebase_restarted;
        m_timebase_restarted = false;
        output_block->input_samples_per_video_sample =
                m_frame_period / NtscInputBlock::c_samples_per_video_line * m_input_samples_decimation_rate;
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv_filled.notify_one();
        m_filled_input_buffers.push_back(std::move(output_block));
        output_block = nullptr;
    }

    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv_filled.notify_one();
    m_reader_thread_finished = true;
}

bool NtscFrameReader::process(std::unique_ptr<NtscInputBlock> const &output_block) {
    auto t_start = std::chrono::steady_clock::now();
    for (;;) {
        if (m_stop_request)
            return false;
        if (consumeFinalized(output_block)) {
            m_process_elapsed_ms += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t_start).count();
            if (++m_timed_frames == c_timing_report_frames) {
                const double frame_budget_ms = NtscInputBlock::c_samples_per_video_line
                        * NtscInputBlock::c_total_video_lines
                        / NtscInputBlock::c_video_sampling_frequency * 1e3;
                m_log.info(ePerformance, std::format(
                        "reader avg/frame (budget {:.2f} ms): timebase+resample {:.2f} ms, input {:.2f} ms",
                        frame_budget_ms,
                        (m_process_elapsed_ms - m_read_input_elapsed_ms) / m_timed_frames,
                        m_read_input_elapsed_ms / m_timed_frames));
                m_process_elapsed_ms = m_read_input_elapsed_ms = 0;
                m_timed_frames = 0;
            }
            return true;
        }
        if (canFinalize()) {
            finalizeBatch();
        } else {
            auto t_read = std::chrono::steady_clock::now();
            bool ok = readInputBlock(output_block);
            m_read_input_elapsed_ms += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t_read).count();
            if (!ok)
                return false;
        }
    }
}

bool NtscFrameReader::readInputBlock(std::unique_ptr<NtscInputBlock> const &output_block) {
    // See ResamplingFrameReader::readInput: a stop has to be noticed here.
    if (m_stop_request)
        return false;

    auto block = m_demodulator->getNextDemodulatedBlock();
    if (block == nullptr) {
        m_log.info(eInput, "NtscFrameReader: no more demodulated blocks");
        return false;
    }

    const int64_t B = (int64_t)c_input_sub_buffer_size;
    // The slot being filled wraps over data four blocks old; finalized lines
    // that still point there have waited too long (cannot happen in normal
    // flow, where lines are resampled one to two blocks after they arrive)
    while (!m_curve.empty() && m_curve.front() < (double)((m_blocks_fetched - 3) * B)) {
        m_log.warn(eInput, std::format("NtscFrameReader: dropping stale line {} (resampling fell behind)",
                                       m_curve_base));
        m_curve.pop_front();
        m_curve_base++;
    }

    const int slot = (int)(m_blocks_fetched % c_number_of_input_sub_buffers);
    float *dst = m_input_buffer + (size_t)slot * c_input_sub_buffer_size;
    memcpy(dst, block->video_data->data<float>(), c_input_sub_buffer_size * sizeof(float));
    memcpy(m_input_dropout_buffer + (size_t)slot * c_input_sub_buffer_size,
           block->dropouts->data<uint8_t>(), c_input_sub_buffer_size);
    m_sub_buffer_input_offsets[slot] = block->input_offset;

    if (output_block != nullptr && m_anchored) {
        output_block->efm_data.insert(output_block->efm_data.end(), block->efm_data.begin(), block->efm_data.end());
        output_block->analog_data.insert(output_block->analog_data.end(), block->analog_data.begin(), block->analog_data.end());
        output_block->ac3_frames.insert(output_block->ac3_frames.end(), block->ac3_frames.begin(), block->ac3_frames.end());
    }
    m_demodulator->returnBlock(block);

    // Debug aid: MUSELD_DUMP_DEMOD=<path> saves the demodulated blocks
    // (float32 at the decimated sample rate) for offline analysis -- the
    // first 32 (about 8 frames) unless MUSELD_DUMP_DEMOD_BLOCKS says
    // otherwise.  Combine with --seek to pick the section.
    static const char *dump_path = getenv("MUSELD_DUMP_DEMOD");
    static int dump_blocks_left = -1;
    if (dump_path != nullptr && dump_blocks_left != 0) {
        const bool first = dump_blocks_left == -1;
        if (first) {
            const char *blocks = getenv("MUSELD_DUMP_DEMOD_BLOCKS");
            dump_blocks_left = blocks != nullptr ? atoi(blocks) : 32;
        }
        if (FILE *f = fopen(dump_path, first ? "wb" : "ab")) {
            fwrite(dst, sizeof(float), c_input_sub_buffer_size, f);
            fclose(f);
        }
        if (--dump_blocks_left == 0)
            m_log.info(eInput, std::format("Demodulated video dump complete: {}", dump_path));
    }

    syncPass(dst, m_blocks_fetched * B, (int)c_input_sub_buffer_size);
    m_blocks_fetched++;
    m_stream_pos += B;

    // Signal-loss guard: with no usable sync pulses for many lines the
    // lattice is stale; start over when they return
    if (m_lattice_valid && (double)m_stream_pos - m_lat_t > 3000 * m_p_nominal) {
        resetTimebase("no sync pulses");
        m_timebase_restarted = true; // (a seek's reset is not a new disc, so it does not set this)
    }

    return true;
}

void NtscFrameReader::syncPass(const float *data, int64_t stream_base, int count) {
    const int ntaps = (int)m_sync_fir.size();
    const size_t in_mask = m_sync_fir_in.size() - 1;
    for (int i = 0; i < count; i++) {
        const int64_t full_ix = stream_base + i;
        m_sync_fir_in[(size_t)full_ix & in_mask] = data[i];
        if (++m_sync_boxcar_phase < c_sync_decim)
            continue;
        m_sync_boxcar_phase = 0;

        float y = 0.f;
        for (int j = 0; j < ntaps; j++)
            y += m_sync_fir[j] * m_sync_fir_in[(size_t)(full_ix - j) & in_mask];
        m_sync_filt[m_sync_dec_count % c_sync_filt_ring] = y;

        if (!m_sync_below && y < c_slice_low) {
            m_sync_below = true;
            m_sync_fall_idx = m_sync_dec_count;
        } else if (m_sync_below && y > c_slice_high) {
            m_sync_below = false;
            const double dec_rate = m_sample_rate / c_sync_decim;
            const double width_us = (double)(m_sync_dec_count - m_sync_fall_idx) / dec_rate * 1e6;
            const bool is_hsync = width_us > c_hsync_width_min && width_us < c_hsync_width_max;
            const bool is_broad = width_us > c_broad_width_min && width_us < c_broad_width_max;
            auto filt = [this](int64_t k) { return m_sync_filt[k % c_sync_filt_ring]; };
            // Blanking level, from the back porch of the previous pulse: a
            // microsecond or more after any pulse's rise the lowpassed
            // signal sits at blanking (the burst averages out) whatever the
            // picture holds.  The front porch is no use for this: the
            // previous line's right edge is smeared into it by the lowpass,
            // and a bright edge there raised the 50% level enough to place
            // the crossing a microsecond early on the smeared slope, for as
            // many lines as the bright edge lasted.  Slow global average, so
            // a dropout there cannot move it.
            if (m_sync_rise_idx > 0 && m_sync_dec_count - m_sync_rise_idx < c_sync_filt_ring - 16) {
                float bp[8];
                for (int j = 0; j < 8; j++)
                    bp[j] = filt(m_sync_rise_idx + 5 + j);
                nth_element(bp, bp + 4, bp + 8);
                if (bp[4] > 0.15f && bp[4] < 0.6f)
                    m_blank_level += 0.02f * (bp[4] - m_blank_level);
            }
            m_sync_rise_idx = m_sync_dec_count;
            if ((is_hsync || is_broad) && m_sync_dec_count - m_sync_fall_idx < c_sync_filt_ring - 24) {
                // 50% crossing between the measured tip and the blanking level
                float tip_buf[40];
                int tn = (int)min<int64_t>(40, m_sync_dec_count - m_sync_fall_idx - 1);
                for (int j = 0; j < tn; j++)
                    tip_buf[j] = filt(m_sync_fall_idx + 1 + j);
                nth_element(tip_buf, tip_buf + tn / 2, tip_buf + tn);
                const float tip = tn > 0 ? tip_buf[tn / 2] : 0.f;
                const float th = 0.5f * (tip + m_blank_level);
                int64_t j = m_sync_fall_idx;
                int steps = 0;
                while (j > 0 && filt(j - 1) < th && ++steps < 16)
                    j--;
                const float a = filt(j - 1), b = filt(j);
                if (a > b) {
                    const double frac = (a - th) / (a - b);
                    // decimated sample d is computed at full-rate index 8d+7
                    const double t = ((double)(j - 1) + frac) * c_sync_decim
                                     + (c_sync_decim - 1) - m_sync_delay;
                    handlePulse(t, width_us);
                }
            }
        }
        m_sync_dec_count++;
    }
}

void NtscFrameReader::handlePulse(double t, double width_us) {
    if (width_us > c_broad_width_min) {
        m_broad_falls.push_back(t);
        if (m_broad_falls.size() > 64)
            m_broad_falls.pop_front();
        return;
    }

    // Lattice assignment by chaining from the previous accepted pulse: the
    // number of lines between two pulses is their measured interval divided
    // by the period, rounded -- unambiguous for gaps of hundreds of lines,
    // since the period is known a priori to 0.1% and tracked here only from
    // clean consecutive intervals.  Crucially the period used for this never
    // comes from the filter.  Assigning line numbers from the filter's own
    // period estimate closes a positive-feedback loop: a small period error
    // pushes real pulses across the assignment boundary, they flip to the
    // next line with a large residual, the fit shortens the period further,
    // and the reconstruction runs away onto a self-consistent false lattice
    // (measured: a 13% short line period settling in a few seconds into the
    // white squall capture and never recovering).
    if (!m_lattice_valid) {
        m_lattice_valid = true;
        m_lat_t = t;
        m_p_run = m_p_nominal;
        m_meas.emplace_back(0, t);
        m_last_meas_k = 0;
        return;
    }
    const double dt = t - m_lat_t;
    const int64_t dk = llround(dt / m_p_run);
    if (dk < 1)
        return; // duplicate or false pulse within the same line
    const double err = abs(dt / m_p_run - (double)dk);
    int64_t k = m_last_meas_k + dk;

    // The curve is consulted only for INTEGER-line disagreement.  A junk
    // pulse can still insert a bogus line into the chain, after which every
    // real pulse sits exactly a line away from where the curve predicts it.
    // Fractional disagreement is never acted on (that is the feedback path
    // above); an integer shift confirmed by several consecutive pulses
    // re-labels the chain and the pulses stored since the slip.  Pulses
    // arrive up to a batch past the look-ahead head, where the extrapolation
    // is good to a small fraction of a line even at the wow extremes; a head
    // gone stale or uncertain after a long gap is not consulted.
    if (m_kal_valid && m_kal_head_k > 0 && k > m_kal_head_k && k - m_kal_head_k <= 2 * c_kal_batch
        && m_kal_head.P00 < pow(0.2 * m_kal_head.p, 2)) {
        const double resid = t - (m_kal_head.t + (double)(k - m_kal_head_k) * m_kal_head.p);
        const int64_t shift = llround(resid / m_kal_head.p);
        if (shift == 0) {
            m_slip_count = 0;
        } else if (m_slip_count == 0 || shift != m_slip_shift) {
            m_slip_shift = shift;
            m_slip_first_k = k;
            m_slip_count = 1;
        } else if (++m_slip_count >= 16) {
            m_log.warn(eInput, std::format("NtscFrameReader: lattice slipped {} line(s) at line {}, re-labeling",
                                           shift, m_slip_first_k));
            for (auto &m : m_meas)
                if (m.first >= m_slip_first_k)
                    m.first += shift;
            m_last_meas_k += shift;
            k += shift;
            m_slip_count = 0;
        }
    }

    if (k <= m_last_meas_k)
        return; // duplicate line
    m_lat_t = t; // last accepted pulse: chain reference and no-sync watchdog
    m_meas.emplace_back(k, t);
    m_last_meas_k = k;
    if (dk == 1 && err < 0.1)
        m_p_run += 0.02 * (dt - m_p_run); // the local period, from clean consecutive intervals only
    // Measurement noise from the data: the second difference of three
    // consecutive-line pulse times has variance 6R plus a negligible wow
    // term.  Mean absolute value, clipped for robustness against junk.
    if (dk == 1 && m_last_dk == 1) {
        const double d2 = abs(dt - m_last_dt);
        m_d2_mean += 0.002 * (min(d2, 5.0 * m_d2_mean) - m_d2_mean);
    }
    m_last_dt = dt;
    m_last_dk = dk;

    if (!m_kal_valid) {
        m_kal_valid = true;
        m_kal.t = m_meas.front().second - m_p_nominal;
        m_kal.p = m_p_nominal;
        m_kal.P00 = 4.0 * m_p_nominal * m_p_nominal;
        m_kal.P01 = 0.0;
        m_kal.P11 = pow(2e-3 * m_p_nominal, 2);
        m_k_final = m_meas.front().first;
        m_curve_base = m_k_final;
    }
}

bool NtscFrameReader::canFinalize() const {
    return m_kal_valid && m_last_meas_k >= m_k_final + c_kal_batch + c_kal_lag;
}

void NtscFrameReader::finalizeBatch() {
    constexpr int W = c_kal_batch + c_kal_lag;
    double meas[W];
    bool has_meas[W] = {};
    for (int i = 0; i < W; i++)
        meas[i] = 0;
    for (auto &[k, t] : m_meas) {
        const int64_t i = k - m_k_final;
        if (i >= 0 && i < W) {
            meas[i] = t;
            has_meas[i] = true;
        }
    }

    // E|d2| = 0.798 sigma_d2 for Gaussian noise, and var(d2) = 6R
    const double sigma_d2 = 1.2533 * m_d2_mean;
    const double R = max(sigma_d2 * sigma_d2 / 6.0, pow(c_sigma_meas_floor_s * m_sample_rate, 2));
    const double q00 = pow(c_sigma_qt_s * m_sample_rate, 2);

    KalState pred[W], filt[W];
    KalState st = m_kal;
    for (int i = 0; i < W; i++) {
        const double qpp = pow(c_sigma_qp_s * m_sample_rate * m_qscale, 2);
        st.t += st.p;
        st.P00 += 2 * st.P01 + st.P11 + q00;
        st.P01 += st.P11;
        st.P11 += qpp;
        pred[i] = st;
        if (has_meas[i]) {
            const double y = meas[i] - st.t;
            const double S = st.P00 + R;
            const double gate2 = c_kal_gate * c_kal_gate * S;
            // A measurement beyond the gate is junk when it stands alone, and
            // the filter's own error when the following pulses agree with it.
            // A hard gate cannot tell the two apart and deadlocks on the
            // second: once the filter has fallen behind (a period slew it was
            // too stiff for, a time-base step), every real pulse is rejected,
            // and the predicted variance of a rejected filter grows far too
            // slowly for the gate to catch up with the runaway innovation --
            // the curve then coasts for the rest of the file.  So an outlier
            // is rejected only when the next measurement fails to corroborate
            // it (a whole number of lines away, by the chain's period).  A
            // corroborated one is absorbed with bounded influence -- the
            // Huber weight inflates its noise so it counts as if it sat at
            // the gate -- which pulls a lost filter back onto the data within
            // tens of lines while two spurious pulses a line apart can only
            // nudge it.
            double R_eff = R;
            bool accept = true;
            if (y * y > gate2) {
                accept = false;
                for (int j = i + 1; j < W && j <= i + 3; j++)
                    if (has_meas[j]) {
                        accept = abs(meas[j] - meas[i] - (double)(j - i) * m_p_run)
                                 < (double)(j - i) * c_corroborate_s * m_sample_rate;
                        break;
                    }
                R_eff = R * sqrt(y * y / gate2);
            }
            if (accept) {
                const double S_eff = st.P00 + R_eff;
                const double K0 = st.P00 / S_eff, K1 = st.P01 / S_eff;
                st.t += K0 * y;
                st.p += K1 * y;
                const double P01_pre = st.P01;
                st.P00 *= (1 - K0);
                st.P01 *= (1 - K0);
                st.P11 -= K1 * P01_pre;
                // Innovation-based adaptation of the period process noise: a
                // filter does not estimate its own noise model, and the wow
                // amplitude varies over the disc (radius, player servo), so
                // the normalized innovation variance steers a slow scale
                if (m_k_final + i > m_adapted_to_k) {
                    m_nis_avg += 0.005 * (min(y * y / S, c_kal_gate * c_kal_gate) - m_nis_avg);
                    if (m_nis_avg > 1.3)
                        m_qscale = min(m_qscale * 1.003, c_qscale_max);
                    else if (m_nis_avg < 0.7)
                        m_qscale = max(m_qscale * 0.998, c_qscale_min);
                }
            }
        }
        filt[i] = st;
    }
    m_adapted_to_k = m_k_final + W - 1;

    // RTS backward pass over the window; only the first c_kal_batch lines are
    // final, the rest is the look-ahead that will be re-run next batch
    KalState sm[W];
    sm[W - 1] = filt[W - 1];
    for (int i = W - 2; i >= 0; i--) {
        const KalState &f = filt[i];
        const KalState &pr = pred[i + 1];
        const double det = pr.P00 * pr.P11 - pr.P01 * pr.P01;
        // A = P_filt * F^T for F = [[1,1],[0,1]]
        const double A00 = f.P00 + f.P01, A01 = f.P01;
        const double A10 = f.P01 + f.P11, A11 = f.P11;
        const double G00 = (A00 * pr.P11 - A01 * pr.P01) / det;
        const double G01 = (-A00 * pr.P01 + A01 * pr.P00) / det;
        const double G10 = (A10 * pr.P11 - A11 * pr.P01) / det;
        const double G11 = (-A10 * pr.P01 + A11 * pr.P00) / det;
        const double dxt = sm[i + 1].t - pr.t, dxp = sm[i + 1].p - pr.p;
        sm[i] = f;
        sm[i].t += G00 * dxt + G01 * dxp;
        sm[i].p += G10 * dxt + G11 * dxp;
    }

    // Debug aid: MUSELD_DUMP_TIMEBASE=<prefix> appends the accepted (k, t)
    // measurements and the finalized curve for offline comparison
    static const char *dump_prefix = getenv("MUSELD_DUMP_TIMEBASE");
    if (dump_prefix != nullptr) {
        if (FILE *f = fopen((std::string(dump_prefix) + ".curve.f64").c_str(), "ab")) {
            for (int i = 0; i < c_kal_batch; i++) {
                double rec[2] = {(double)(m_k_final + i), sm[i].t};
                fwrite(rec, sizeof rec, 1, f);
            }
            fclose(f);
        }
        if (FILE *f = fopen((std::string(dump_prefix) + ".meas.f64").c_str(), "ab")) {
            for (auto &[k, t] : m_meas)
                if (k >= m_k_final && k < m_k_final + c_kal_batch) {
                    double rec[2] = {(double)k, t};
                    fwrite(rec, sizeof rec, 1, f);
                }
            fclose(f);
        }
    }

    // Per-frame sync quality: over the lines being finalized, count how many
    // carried a sync and how many of those disagree with the smoothed curve
    // by more than 2 us.  This reports on the timebase without ever looking
    // at the decoded picture -- a raw-vs-reconstructed check, computed
    // against the final curve rather than the forward filter's coasting
    // prediction.  (No step detector lives here any more: analysis of the
    // raw pulse stream showed the disturbances on even the worst disc are
    // smooth and continuous -- wow and crosstalk bias, nothing discontinuous
    // -- and a genuine time-base step recovers by itself, the rejections
    // growing the predicted variance until the widened gate absorbs it.)
    for (int i = 0; i < c_kal_batch; i++)
        m_curve.push_back(sm[i].t);
    // A finalized line is "supported" when a detected pulse falls within 2 us
    // of its curve position; the unsupported ones are where sync is genuinely
    // missing or corrupt and the picture can weave.  (Counting instead every
    // measurement that disagrees with the curve would be backwards: the junk
    // and half-line pulses far from the curve are exactly what the gate is
    // meant to reject, and rejecting them is the curve working, not failing.)
    bool supported[c_kal_batch] = {};
    for (auto &[k, t] : m_meas)
        if (k >= m_k_final && k < m_k_final + c_kal_batch
            && abs(t - sm[k - m_k_final].t) < 2.0e-6 * m_sample_rate)
            supported[k - m_k_final] = true;
    for (int i = 0; i < c_kal_batch; i++) {
        m_frame_meas++;
        if (!supported[i])
            m_frame_resid_bad++;
    }
    m_kal_head = filt[W - 1];
    m_kal_head_k = m_k_final + W - 1;
    m_kal = filt[c_kal_batch - 1];
    m_k_final += c_kal_batch;

    while (m_k_final - m_frame_log_k >= NtscInputBlock::c_total_video_lines) {
        m_frame_log_k += NtscInputBlock::c_total_video_lines;
        m_log.debug(eInput, std::format("timebase to line {}: {} of {} lines unsupported by a nearby sync pulse; "
                                        "sigma_meas {:.1f} ns, qscale {:.2f}",
                                        m_frame_log_k, m_frame_resid_bad, m_frame_meas,
                                        sqrt(R) / m_sample_rate * 1e9, m_qscale));
        m_frame_resid_bad = 0;
        m_frame_meas = 0;
    }

    while (!m_meas.empty() && m_meas.front().first < m_k_final)
        m_meas.pop_front();

    evaluateAnchors();
}

void NtscFrameReader::evaluateAnchors() {
    if (m_curve.size() < 2)
        return;
    const double t_lo = m_curve.front();
    const double t_hi = m_curve.back();

    while (!m_broad_falls.empty() && m_broad_falls.front() < t_lo)
        m_broad_falls.pop_front();

    // A vertical sync group is a run of broad pulses at half-line spacing;
    // its position on the lattice identifies the field: field 1's group runs
    // from the top of line 4 (a line boundary) to the middle of line 6,
    // field 2's from the middle of line 266 to the top of line 269.
    size_t i = 0;
    while (i < m_broad_falls.size()) {
        // the group's extent (consecutive pulses at half-line spacing)
        size_t j = i;
        while (j + 1 < m_broad_falls.size()
               && m_broad_falls[j + 1] - m_broad_falls[j] > 0.35 * m_p_run
               && m_broad_falls[j + 1] - m_broad_falls[j] < 0.65 * m_p_run)
            j++;
        if (m_broad_falls[j] >= t_hi)
            break; // not yet covered by the curve; revisit after the next batch
        const double t0 = m_broad_falls[i];
        const double t_last = m_broad_falls[j];
        const size_t group_len = j - i + 1;
        m_broad_falls.erase(m_broad_falls.begin() + (ptrdiff_t)i,
                            m_broad_falls.begin() + (ptrdiff_t)j + 1);
        if (group_len < 5)
            continue;

        // Phase each end of the group against the lattice.  Field 1's six
        // broad pulses run from the top of line 4 to the middle of line 6,
        // field 2's from the middle of line 266 to the top of line 269.  A
        // missed pulse at either end would flip that end's verdict, so the
        // anchor is only trusted when both ends agree on the field.
        auto locate = [this](double t, int64_t *k_out, double *phase_out) -> bool {
            size_t lo = 0, hi = m_curve.size() - 1;
            while (lo + 1 < hi) {
                const size_t mid = (lo + hi) / 2;
                (m_curve[mid] <= t ? lo : hi) = mid;
            }
            if (m_curve[lo] > t || m_curve[lo + 1] <= t)
                return false;
            *k_out = m_curve_base + (int64_t)lo;
            *phase_out = (t - m_curve[lo]) / (m_curve[lo + 1] - m_curve[lo]);
            return true;
        };
        int64_t k_first, k_last;
        double ph_first, ph_last;
        if (!locate(t0, &k_first, &ph_first) || !locate(t_last, &k_last, &ph_last))
            continue;
        // Each end's phase must sit close to its ideal (a line boundary or
        // the middle of a line): through a rough stretch the fitted curve
        // can wander a fraction of a line off the true grid, and a group
        // phased in no-man's-land is exactly such a stretch talking -- not
        // usable for anchoring.
        const bool first_on_boundary = ph_first < 0.15 || ph_first > 0.85;
        const bool last_on_boundary = ph_last < 0.15 || ph_last > 0.85;
        const bool first_mid = abs(ph_first - 0.5) < 0.15;
        const bool last_mid = abs(ph_last - 0.5) < 0.15;
        if (!((first_on_boundary && last_mid) || (first_mid && last_on_boundary)))
            continue;
        int64_t cand;
        if (first_on_boundary) {
            if (ph_first > 0.85)
                k_first++;
            cand = k_first - 3;   // field 1: group starts at the top of line 4
        } else {
            cand = k_first - 265; // field 2: group starts mid line 266
        }

        if (!m_anchored) {
            m_line1_k = cand;
            while (m_line1_k < m_curve_base)
                m_line1_k += NtscInputBlock::c_total_video_lines;
            m_anchored = true;
            m_pending_drift = 0;
            m_log.info(eInput, std::format("NtscFrameReader: anchored, frame starts at lattice line {}", m_line1_k));
        } else {
            int64_t drift = (cand - m_line1_k) % NtscInputBlock::c_total_video_lines;
            if (drift > NtscInputBlock::c_total_video_lines / 2)
                drift -= NtscInputBlock::c_total_video_lines;
            if (drift < -NtscInputBlock::c_total_video_lines / 2)
                drift += NtscInputBlock::c_total_video_lines;
            if (drift == 0) {
                m_pending_drift = 0;
            } else if (drift != m_pending_drift) {
                // require two consecutive groups to agree before re-anchoring:
                // a single odd group is noise, a real slip confirms next field
                m_pending_drift = drift;
            } else {
                m_log.warn(eInput, std::format("NtscFrameReader: vertical drift {} lines, re-anchoring", drift));
                m_line1_k = cand;
                while (m_line1_k < m_curve_base)
                    m_line1_k += NtscInputBlock::c_total_video_lines;
                m_pending_drift = 0;
            }
        }
    }
}

int64_t NtscFrameReader::inputOffsetOfStreamPos(double stream_pos) const {
    const int64_t B = (int64_t)c_input_sub_buffer_size;
    const int64_t p = (int64_t)stream_pos;
    const int slot = (int)((p / B) % c_number_of_input_sub_buffers);
    return m_sub_buffer_input_offsets[slot] + (p % B) * m_input_samples_decimation_rate;
}

bool NtscFrameReader::consumeFinalized(std::unique_ptr<NtscInputBlock> const &output_block) {
    const int64_t B = (int64_t)c_input_sub_buffer_size;
    while (m_curve.size() >= 2) {
        const double t0 = m_curve[0], t1 = m_curve[1];
        if (t1 + 3 >= (double)m_stream_pos)
            return false; // resampling needs samples that have not arrived yet
        const double min_valid = (double)((m_blocks_fetched - c_number_of_input_sub_buffers) * B + 4);
        bool resampled_last = false;
        if (t0 >= min_valid && m_anchored) {
            if (m_curve_base - m_line1_k + 1 > NtscInputBlock::c_total_video_lines)
                m_line1_k += NtscInputBlock::c_total_video_lines;
            const int64_t ntsc_line = m_curve_base - m_line1_k + 1;
            if (ntsc_line >= 1 && ntsc_line <= NtscInputBlock::c_total_video_lines) {
                if (ntsc_line == 1) {
                    m_frame_start_offset = inputOffsetOfStreamPos(t0);
                    m_frame_period = t1 - t0;
                }
                resampleLine(output_block, (int)ntsc_line, t0, t1);
                resampled_last = ntsc_line == NtscInputBlock::c_total_video_lines;
            }
        }
        m_curve.pop_front();
        m_curve_base++;
        if (resampled_last)
            return true;
    }
    return false;
}

void NtscFrameReader::resampleLine(std::unique_ptr<NtscInputBlock> const &output_block,
                                   int row, double t0, double t1) {
    float *out = output_block->video_data->data<float>()
                 + (size_t)NtscInputBlock::c_samples_per_video_line * (row - 1);
    uint8_t *out_do = output_block->dropout_data->data<uint8_t>()
                      + (size_t)NtscInputBlock::c_samples_per_video_line * (row - 1);
    const double step = (t1 - t0) / NtscInputBlock::c_samples_per_video_line;
    double pos = t0;
    for (int j = 0; j < NtscInputBlock::c_samples_per_video_line; j++, pos += step) {
        const int64_t ip = (int64_t)pos;
        const float frac = (float)(pos - (double)ip);
        const size_t i1 = (size_t)ip & c_input_buffer_size_mask;
        const size_t i0 = (size_t)(ip - 1) & c_input_buffer_size_mask;
        const size_t i2 = (size_t)(ip + 1) & c_input_buffer_size_mask;
        const size_t i3 = (size_t)(ip + 2) & c_input_buffer_size_mask;
        out[j] = cubicInterpolate(m_input_buffer[i0], m_input_buffer[i1],
                                  m_input_buffer[i2], m_input_buffer[i3], frac);
        out_do[j] = m_input_dropout_buffer[frac < 0.5f ? i1 : i2];
    }
}
