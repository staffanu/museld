// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "input/ByteSource.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <cassert>
#include <filesystem>
#include "musevk/VulkanBuffer.h"
#include "SdtvFrameReader.h"

#include "SdtvRfDemodulator.h"
#include "filter/WindowedSinc.h"
#include "logging/Logger.h"
#include "util/Interpolate.h"
#include "timebase/LineTimebase.h"

using namespace std;

namespace {
    // Pulse widths on the lowpassed signal, in microseconds.  Equalizing
    // pulses (2.3 us) come out at 2.5-3.0 us after the lowpass and the
    // hysteresis, hsync (4.7 us) at 4.5-5.5: the minimum sits between
    // them with a margin on both sides, since an equalizing pulse taken
    // for an hsync lands half a line off and derails the lattice chain
    constexpr double c_hsync_width_min = 3.5, c_hsync_width_max = 6.5;
    constexpr double c_broad_width_min = 20.0, c_broad_width_max = 32.0;
    // Hysteresis slicer levels (the demodulated video is normalized: sync tip
    // ~0.0, blanking ~0.3)
    constexpr float c_slice_low = 0.12f, c_slice_high = 0.18f;
    constexpr float c_blank_nominal = 0.30f;
}

SdtvFrameReader::SdtvFrameReader(
        Logger &log, const std::string &executable_dir, musevk::VulkanManager &vulkan_manager,
        PreparedInput input, double sample_rate,
        double initial_seek_seconds, bool benchmark_shaders, AudioTrack audio_track,
        int efm_adaptive_filter_size, const std::optional<std::string> &output_filename,
        const VideoStandard &video_standard)
        : FrameReader(log, input.name(),
                      input.isLive(),
                      initial_seek_seconds, output_filename),
          m_video_standard(video_standard),
          m_demodulator(nullptr),
          m_input_sample_rate(sample_rate),
          m_sample_rate(sample_rate / SdtvRfDemodulatorConstants::c_video_decimation_rate),
          m_input_samples_decimation_rate(SdtvRfDemodulatorConstants::c_video_decimation_rate),
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
          m_sync_fall_frac(0),
          m_sync_rise_idx(0),
          m_blank_level(c_blank_nominal),
          m_blank_local(c_blank_nominal),
          m_timebase(log, "SdtvFrameReader", sample_rate / SdtvRfDemodulatorConstants::c_video_decimation_rate,
                     sample_rate / SdtvRfDemodulatorConstants::c_video_decimation_rate / video_standard.line_hz,
                     video_standard.total_lines),
          m_pilot_valid(false),
          m_pilot_phase_ref(0),
          m_pilot_amp_ref(0),
          m_pilot_corr_next(0),
          m_pilot_corr_next_valid(false),
          m_anchored(false),
          m_timebase_restarted(false),
          m_seek_generation(0),
          m_frame_partial(true),
          m_line1_k(0),
          m_pending_drift(0),
          m_frame_start_offset(0),
          m_frame_period(0),
          m_process_elapsed_ms(0),
          m_read_input_elapsed_ms(0),
          m_timed_frames(0) {
    m_demodulator = new SdtvRfDemodulator(log, executable_dir, std::move(input), sample_rate, vulkan_manager,
                                          benchmark_shaders, audio_track, efm_adaptive_filter_size,
                                          video_standard);
}

bool SdtvFrameReader::initialize(std::vector<std::unique_ptr<SdtvInputBlock>> &buffers) {
    m_demodulator->initialize(m_demodulator->numberOfBlockBuffers());

    m_p_nominal = m_sample_rate / m_video_standard.line_hz;

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
void SdtvFrameReader::cleanup() {
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

// The seeks only move the demodulator's input: the timebase is reset on the
// reader thread when the first block after the seek arrives (readInputBlock
// sees its generation change), so the reset never races the thread's own
// use of the timebase state, and the blocks still in flight are consumed
// under the old timebase and delivered with the old generation.
bool SdtvFrameReader::seek(double seconds) {
    if (m_input_is_realtime)
        return true;
    if (!m_demodulator->seek(seconds))
        return false;
    discardFilledBuffers();
    return true;
}

std::optional<uint32_t> SdtvFrameReader::seekToInputSample(int64_t sample) {
    if (m_input_is_realtime)
        return std::nullopt;
    const auto generation = m_demodulator->seekToSample(sample);
    if (generation)
        discardFilledBuffers();
    return generation;
}

int64_t SdtvFrameReader::inputSampleCount() const {
    return m_demodulator->inputSampleCount();
}

void SdtvFrameReader::setAudioTrack(AudioTrack track) {
    // The audio tracks are alternatives: only the selected one is demodulated
    if (m_demodulator != nullptr)
        m_demodulator->setAudioTrack(track);
}

void SdtvFrameReader::setEfmAdaptiveFilterSize(int size) {
    if (m_demodulator != nullptr)
        m_demodulator->setEfmAdaptiveFilterSize(size);
}

int SdtvFrameReader::efmAdaptiveFilterSize() const {
    return m_demodulator != nullptr ? m_demodulator->efmAdaptiveFilterSize() : -1;
}

void SdtvFrameReader::setAnalogCx(bool enabled) {
    if (m_demodulator != nullptr)
        m_demodulator->setAnalogCx(enabled);
}

void SdtvFrameReader::resetTimebase(const char *why) {
    m_timebase.reset();
    m_broad_falls.clear();
    m_pilot_valid = false;
    m_pilot_corr_next_valid = false;
    m_anchored = false;
    m_pending_drift = 0;
    m_frame_partial = true;
    m_log.info(eInput, std::format("SdtvFrameReader: timebase reset ({})", why));
}

void SdtvFrameReader::threadFunc() {
    unique_ptr<SdtvInputBlock> output_block = nullptr;

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
                m_log.info(eInput, "SdtvFrameReader: stop requested");
                break;
            }
            output_block = std::move(m_vacant_input_buffers.front());
            m_vacant_input_buffers.pop_front();
            output_block->efm_data.clear();
            output_block->analog_data.clear();
            output_block->ac3_frames.clear();
        }

        if (!process(output_block)) {
            m_log.info(eInput, m_stop_request ? "SdtvFrameReader: stop requested"
                                              : "SdtvFrameReader: end of file");
            break;
        }

        output_block->input_offset = m_frame_start_offset;
        output_block->seek_generation = m_seek_generation;
        output_block->timebase_restarted = m_timebase_restarted;
        m_timebase_restarted = false;
        output_block->input_samples_per_video_sample =
                m_frame_period / m_video_standard.samples_per_line * m_input_samples_decimation_rate;
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv_filled.notify_one();
        m_filled_input_buffers.push_back(std::move(output_block));
        output_block = nullptr;
    }

    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv_filled.notify_one();
    m_reader_thread_finished = true;
}

bool SdtvFrameReader::process(std::unique_ptr<SdtvInputBlock> const &output_block) {
    auto t_start = std::chrono::steady_clock::now();
    for (;;) {
        if (m_stop_request)
            return false;
        if (consumeFinalized(output_block)) {
            m_process_elapsed_ms += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t_start).count();
            if (++m_timed_frames == c_timing_report_frames) {
                const double frame_budget_ms = m_video_standard.frameDurationMs();
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
        if (m_timebase.canFinalize()) {
            m_timebase.finalizeBatch();
            evaluateAnchors();
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

bool SdtvFrameReader::readInputBlock(std::unique_ptr<SdtvInputBlock> const &output_block) {
    // A stop has to be noticed here, not only between blocks.
    if (m_stop_request)
        return false;

    auto block = m_demodulator->getNextDemodulatedBlock();
    if (block == nullptr) {
        m_log.info(eInput, "SdtvFrameReader: no more demodulated blocks");
        return false;
    }

    const int64_t B = (int64_t)c_input_sub_buffer_size;
    // The slot being filled wraps over data four blocks old; finalized lines
    // that still point there have waited too long (cannot happen in normal
    // flow, where lines are resampled one to two blocks after they arrive)
    while (m_timebase.curveSize() > 0 && m_timebase.curve(0) < (double)((m_blocks_fetched - 3) * B)) {
        m_log.warn(eInput, std::format("SdtvFrameReader: dropping stale line {} (resampling fell behind)",
                                       m_timebase.curveBase()));
        m_timebase.popLine();
    }

    if (block->seek_generation != m_seek_generation) {
        m_seek_generation = block->seek_generation;
        resetTimebase("seek");
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
        // and the dropout flags of the same blocks, one byte per sample, to <path>.do
        if (FILE *f = fopen((std::string(dump_path) + ".do").c_str(), first ? "wb" : "ab")) {
            fwrite(m_input_dropout_buffer + (size_t)slot * c_input_sub_buffer_size, 1, c_input_sub_buffer_size, f);
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
    if (m_timebase.latticeValid() && (double)m_stream_pos - m_timebase.lastPulseTime() > 3000 * m_p_nominal) {
        resetTimebase("no sync pulses");
        m_timebase_restarted = true; // (a seek's reset is not a new disc, so it does not set this)
    }

    return true;
}

void SdtvFrameReader::syncPass(const float *data, int64_t stream_base, int count) {
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
        const float y_prev = m_sync_dec_count > 0 ? m_sync_filt[(m_sync_dec_count - 1) % c_sync_filt_ring] : y;
        m_sync_filt[m_sync_dec_count % c_sync_filt_ring] = y;

        if (!m_sync_below && y < c_slice_low) {
            m_sync_below = true;
            m_sync_fall_idx = m_sync_dec_count;
            // The crossing's fractional position, for the width: in whole
            // decimated samples (0.5 us at a 30 MHz capture) an equalizing
            // pulse could round up to the hsync minimum
            m_sync_fall_frac = y_prev > y ? (y_prev - c_slice_low) / (y_prev - y) : 1.0;
        } else if (m_sync_below && y > c_slice_high) {
            m_sync_below = false;
            const double dec_rate = m_sample_rate / c_sync_decim;
            const double rise_frac = y > y_prev ? (c_slice_high - y_prev) / (y - y_prev) : 1.0;
            const double width_us = ((double)(m_sync_dec_count - m_sync_fall_idx) + rise_frac - m_sync_fall_frac)
                                    / dec_rate * 1e6;
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
            // a dropout there cannot move it.  The window is in time, not
            // decimated samples: the back porch ends 4.7 us after the rise,
            // and a sample count that fit at 62.5 MHz reached into the
            // picture at 30 MHz, where the level then followed the picture's
            // left edge down the field and took the 50% crossings with it
            // (a 0.3 us sawtooth per field, and slips at the worst lines).
            const int bp_first = (int)lround(1.5e-6 * dec_rate);
            const int bp_count = clamp((int)lround(2.5e-6 * dec_rate), 3, 16);
            if (m_sync_rise_idx > 0 && m_sync_dec_count - m_sync_rise_idx < c_sync_filt_ring - 16
                && m_sync_dec_count - m_sync_rise_idx > bp_first + bp_count) {
                float bp[16];
                for (int j = 0; j < bp_count; j++)
                    bp[j] = filt(m_sync_rise_idx + bp_first + j);
                nth_element(bp, bp + bp_count / 2, bp + bp_count);
                const float level = bp[bp_count / 2];
                if (level > 0.15f && level < 0.6f)
                    m_blank_level += 0.02f * (level - m_blank_level);
                // The threshold follows the porch just measured, not the
                // slow average: a CAV capture with heavy eccentricity (the
                // WOW demo disc) had its carrier, and the whole baseband,
                // moving +-4 IRE over each rotation, and the average's lag
                // put the 50% crossings +-50 ns off in step with it -- the
                // two fields displaced by a pixel against each other.  The
                // average remains the sanity check: a porch far from it
                // (a dropout) is not used.
                m_blank_local = std::abs(level - m_blank_level) < 0.07f ? level : m_blank_level;
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
                const float th = 0.5f * (tip + m_blank_local);
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

void SdtvFrameReader::handlePulse(double t, double width_us) {
    if (width_us > c_broad_width_min) {
        m_broad_falls.push_back(t);
        if (m_broad_falls.size() > 64)
            m_broad_falls.pop_front();
        return;
    }
    m_timebase.addPulse(t);
}

void SdtvFrameReader::evaluateAnchors() {
    if (m_timebase.curveSize() < 2)
        return;
    const double t_lo = m_timebase.curve(0);
    const double t_hi = m_timebase.curve(m_timebase.curveSize() - 1);

    while (!m_broad_falls.empty() && m_broad_falls.front() < t_lo)
        m_broad_falls.pop_front();

    // A vertical sync group is a run of broad pulses at half-line spacing;
    // its position on the lattice identifies the field: field 1's group runs
    // from the top of line 4 (a line boundary) to the middle of line 6,
    // field 2's from the middle of line 266 to the top of line 269.  (NTSC
    // numbering; PAL counts its lines from the vertical sync, so its five
    // broad pulses run from the top of line 1, and field 2's from the middle
    // of line 313.)
    const int broad_start = m_video_standard.vsync_broad_start_line;
    const int broad_pulses = m_video_standard.vsync_broad_pulses;
    size_t i = 0;
    while (i < m_broad_falls.size()) {
        // the group's extent (consecutive pulses at half-line spacing)
        size_t j = i;
        while (j + 1 < m_broad_falls.size()
               && m_broad_falls[j + 1] - m_broad_falls[j] > 0.35 * m_timebase.period()
               && m_broad_falls[j + 1] - m_broad_falls[j] < 0.65 * m_timebase.period())
            j++;
        if (m_broad_falls[j] >= t_hi)
            break; // not yet covered by the curve; revisit after the next batch
        const double t0 = m_broad_falls[i];
        const double t_last = m_broad_falls[j];
        const size_t group_len = j - i + 1;
        m_broad_falls.erase(m_broad_falls.begin() + (ptrdiff_t)i,
                            m_broad_falls.begin() + (ptrdiff_t)j + 1);
        if (group_len < (size_t)broad_pulses - 1)
            continue;

        // Phase each end of the group against the lattice.  A missed pulse
        // at either end would flip that end's verdict, so the anchor is only
        // trusted when both ends agree on the field.
        auto locate = [this](double t, int64_t *k_out, double *phase_out) { return m_timebase.locate(t, k_out, phase_out); };
        int64_t k_first, k_last;
        double ph_first, ph_last;
        if (!locate(t0, &k_first, &ph_first) || !locate(t_last, &k_last, &ph_last))
            continue;
        // Each end's phase must sit close to its ideal (a line boundary or
        // the middle of a line): through a rough stretch the fitted curve
        // can wander a fraction of a line off the true grid, and a group
        // phased in no-man's-land is exactly such a stretch talking -- not
        // usable for anchoring.  The last pulse falls (broad_pulses - 1)
        // half lines after the first: NTSC's six pulses put the two ends on
        // different phases, PAL's five on the same one -- and a group that
        // lost an end pulse shows the other pattern, which is what rejects
        // it (classifying such a group by its surviving end would label the
        // fields the wrong way round half the time).
        const bool first_on_boundary = ph_first < 0.15 || ph_first > 0.85;
        const bool last_on_boundary = ph_last < 0.15 || ph_last > 0.85;
        const bool first_mid = abs(ph_first - 0.5) < 0.15;
        const bool last_mid = abs(ph_last - 0.5) < 0.15;
        const bool ends_alike = broad_pulses % 2 == 1;
        const bool field1 = first_on_boundary && (ends_alike ? last_on_boundary : last_mid);
        const bool field2 = first_mid && (ends_alike ? last_mid : last_on_boundary);
        if (!field1 && !field2)
            continue;
        int64_t cand;
        if (field1) {
            if (ph_first > 0.85)
                k_first++;
            cand = k_first - (broad_start - 1);   // field 1: group starts at the top of line broad_start
        } else {
            // field 2: group starts mid line broad_start + field2_offset - 1
            cand = k_first - (broad_start + m_video_standard.field2_offset - 2);
        }

        if (!m_anchored) {
            m_line1_k = cand;
            while (m_line1_k < m_timebase.curveBase())
                m_line1_k += m_video_standard.total_lines;
            m_anchored = true;
            m_pending_drift = 0;
            m_log.info(eInput, std::format("SdtvFrameReader: anchored, frame starts at lattice line {}", m_line1_k));
        } else {
            int64_t drift = (cand - m_line1_k) % m_video_standard.total_lines;
            if (drift > m_video_standard.total_lines / 2)
                drift -= m_video_standard.total_lines;
            if (drift < -m_video_standard.total_lines / 2)
                drift += m_video_standard.total_lines;
            if (drift == 0) {
                m_pending_drift = 0;
            } else if (drift != m_pending_drift) {
                // require two consecutive groups to agree before re-anchoring:
                // a single odd group is noise, a real slip confirms next field
                m_pending_drift = drift;
            } else {
                m_log.warn(eInput, std::format("SdtvFrameReader: vertical drift {} lines, re-anchoring", drift));
                m_line1_k = cand;
                while (m_line1_k < m_timebase.curveBase())
                    m_line1_k += m_video_standard.total_lines;
                m_pending_drift = 0;
            }
        }
    }
}

int64_t SdtvFrameReader::inputOffsetOfStreamPos(double stream_pos) const {
    const int64_t B = (int64_t)c_input_sub_buffer_size;
    const int64_t p = (int64_t)stream_pos;
    const int slot = (int)((p / B) % c_number_of_input_sub_buffers);
    return m_sub_buffer_input_offsets[slot] + (p % B) * m_input_samples_decimation_rate;
}

bool SdtvFrameReader::consumeFinalized(std::unique_ptr<SdtvInputBlock> const &output_block) {
    const int64_t B = (int64_t)c_input_sub_buffer_size;
    while (m_timebase.curveSize() >= 2) {
        double t0 = m_timebase.curve(0), t1 = m_timebase.curve(1);
        if (t1 + 3 >= (double)m_stream_pos)
            return false; // resampling needs samples that have not arrived yet
        const double min_valid = (double)((m_blocks_fetched - c_number_of_input_sub_buffers) * B + 4);
        // The pilot's refinement of both ends (the far end's is reused as
        // the near end's of the next line)
        if (m_video_standard.pilot_hz > 0 && t0 >= min_valid) {
            const double c0 = m_pilot_corr_next_valid ? m_pilot_corr_next : pilotCorrection(t0);
            const double c1 = pilotCorrection(t1);
            m_pilot_corr_next = c1;
            m_pilot_corr_next_valid = true;
            t0 += c0;
            t1 += c1;
            // Debug aid, with MUSELD_DUMP_TIMEBASE: the corrected curve, for
            // tools/pal-pilot-check.py to measure the residual
            static const char *dump_prefix = getenv("MUSELD_DUMP_TIMEBASE");
            if (dump_prefix != nullptr) {
                if (FILE *f = fopen((std::string(dump_prefix) + ".pilot.f64").c_str(), "ab")) {
                    double rec[2] = {(double)m_timebase.curveBase(), t0};
                    fwrite(rec, sizeof rec, 1, f);
                    fclose(f);
                }
            }
        }
        bool resampled_last = false;
        if (t0 >= min_valid && m_anchored) {
            if (m_timebase.curveBase() - m_line1_k + 1 > m_video_standard.total_lines)
                m_line1_k += m_video_standard.total_lines;
            const int64_t ntsc_line = m_timebase.curveBase() - m_line1_k + 1; // the standard's line number
            if (ntsc_line >= 1 && ntsc_line <= m_video_standard.total_lines) {
                if (ntsc_line == 1) {
                    m_frame_start_offset = inputOffsetOfStreamPos(t0);
                    m_frame_period = t1 - t0;
                    if (m_frame_partial) {
                        // The audio gathered since the anchoring belongs to
                        // the partial frame dropped here, not to this one:
                        // delivered with it, every anchor and reset would
                        // leave a frame's worth of audio queued ahead of the
                        // picture (the player drains such a backlog faster
                        // than playback can absorb, and discards for seconds)
                        output_block->efm_data.clear();
                        output_block->analog_data.clear();
                        output_block->ac3_frames.clear();
                    }
                    m_frame_partial = false;
                }
                resampleLine(output_block, (int)ntsc_line, t0, t1);
                resampled_last = ntsc_line == m_video_standard.total_lines;
            }
        }
        m_timebase.popLine();
        // A frame whose first rows predate the anchoring (the start of the
        // input, a seek, a re-acquired signal) would show half of one
        // picture and half of another, and report the offset and VBI codes
        // of the wrong one: wait for the next whole frame instead
        if (resampled_last && !m_frame_partial)
            return true;
    }
    return false;
}

double SdtvFrameReader::pilotCorrection(double t) {
    // Correlate the window 0.8-3.8 us after the line start (inside the
    // pilot's 0.5-4.1 us, clear of the filtered edges) against the pilot
    // frequency, phased to the line start itself
    const double w = 2 * M_PI * m_video_standard.pilot_hz / m_sample_rate; // radians per sample
    const int n0 = (int)(0.8e-6 * m_sample_rate), n1 = (int)(3.8e-6 * m_sample_rate);
    const int64_t base = (int64_t)floor(t);
    const double frac = t - (double)base;
    double re = 0, im = 0, mean = 0;
    for (int n = n0; n < n1; n++)
        mean += m_input_buffer[(size_t)(base + n) & c_input_buffer_size_mask];
    mean /= (n1 - n0);
    for (int n = n0; n < n1; n++) {
        const double x = m_input_buffer[(size_t)(base + n) & c_input_buffer_size_mask] - mean;
        const double ph = w * ((double)n - frac);
        re += x * cos(ph);
        im -= x * sin(ph);
    }
    const double amp = hypot(re, im) / (n1 - n0);
    const double phase = atan2(im, re);
    // Seed the references from the first lines, then track slowly: the
    // reference phase follows mastering drift over hundreds of lines, never
    // the line-to-line jitter this exists to remove; a line whose pilot is
    // weak (a dropout through the tip, the vertical interval where the
    // half-line bursts are optional) gets no correction and does not move
    // the references
    if (!m_pilot_valid) {
        m_pilot_phase_ref = phase;
        m_pilot_amp_ref = amp;
        m_pilot_valid = true;
        return 0;
    }
    if (amp < 0.6 * m_pilot_amp_ref)
        return 0;
    m_pilot_amp_ref += 0.01 * (amp - m_pilot_amp_ref);
    const double dev = remainder(phase - m_pilot_phase_ref, 2 * M_PI);
    m_pilot_phase_ref = remainder(m_pilot_phase_ref + 0.01 * dev, 2 * M_PI);
    // A pilot that arrives later than the line start it is phased to has a
    // more negative phase; the start is that much later
    const double correction = -dev / w;
    return clamp(correction, -0.25 * 2 * M_PI / w, 0.25 * 2 * M_PI / w);
}

void SdtvFrameReader::resampleLine(std::unique_ptr<SdtvInputBlock> const &output_block,
                                   int row, double t0, double t1) {
    const int width = m_video_standard.samples_per_line;
    float *out = output_block->video_data->data<float>() + (size_t)width * (row - 1);
    uint8_t *out_do = output_block->dropout_data->data<uint8_t>() + (size_t)width * (row - 1);
    const double step = (t1 - t0) / width;
    double pos = t0;
    for (int j = 0; j < width; j++, pos += step) {
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
