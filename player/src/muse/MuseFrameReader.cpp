// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <cassert>
#include "musevk/VulkanBuffer.h"
#include "MuseFrameReader.h"
#include "input/InputReaderFactory.h"
#include "input/ByteSource.h"
#include "logging/Logger.h"
#include "MuseConstants.h"
#include "MuseInputBlock.h"
#include "util/Interpolate.h"

using namespace std;

namespace {
    constexpr double c_pixel_hz = 16.2e6;
    constexpr double c_line_hz = c_pixel_hz / MUSE_TOTAL_WIDTH;
    // The HD sync's plateau difference on the 0..255 scale (measured 121 on
    // every capture; the levels are fixed by the standard), and how far a
    // candidate edge may stray from the tracked value
    constexpr float c_amp_nominal = 121.f;
    constexpr float c_amp_min = 0.5f, c_amp_max = 1.6f;
    constexpr float c_flatness = 0.35f;     // plateau probe difference, as a fraction of the amplitude
    // The plateau levels themselves (measured 57 and 178; wide windows, the
    // frame pulses are the real test of a chain)
    constexpr float c_level_lo_min = 20.f, c_level_lo_max = 100.f;
    constexpr float c_level_hi_min = 135.f, c_level_hi_max = 220.f;
    constexpr int c_chain_lock = 32;        // lines before a chain is trusted enough to verify
    constexpr int c_chain_max = 1200;       // a chain that long without a frame start is not the sync
    constexpr int c_chain_misses = 2;       // lines a chain survives without its edge
    constexpr int c_chain_alternation_check = 8; // consecutive-line steps before the polarity is judged
    constexpr size_t c_max_chains = 256;
    constexpr int c_frames_without_pulses = 4; // frames whose lines 1-2 lack the frame pulses before a restart
}

MuseFrameReader::MuseFrameReader(
        Logger &log, const std::string &executable_dir, musevk::VulkanManager &vulkan_manager,
        PreparedInput input, double sample_rate,
        double initial_seek_seconds, bool demodulate, bool benchmark_shaders, bool efm_enabled,
        int efm_adaptive_filter_size, const std::optional<std::string> &output_filename)
        : FrameReader(log, input.name(),
                      input.isLive(),
                      initial_seek_seconds, output_filename),
          m_input(std::move(input)),
          m_demodulate(demodulate),
          m_input_reader(nullptr),
          m_sample_rate(demodulate ? sample_rate / MuseDemodulatedBlock::c_video_decimation_rate : sample_rate),
          m_input_samples_decimation_rate(demodulate ? MuseDemodulatedBlock::c_video_decimation_rate : 1),
          m_demodulator(nullptr),
          m_input_scale(1.f),
          m_input_offset(0.f),
          m_file_reset_request(false),
          m_input_buffer(nullptr),
          m_input_dropout_buffer(nullptr),
          m_sub_buffer_input_offsets{},
          m_blocks_fetched(0),
          m_stream_pos(0),
          m_pix(m_sample_rate / c_pixel_hz),
          m_p_nominal(m_sample_rate / c_line_hz),
          m_off_a0(0), m_off_a1(0), m_off_b0(0), m_off_b1(0), m_off_mid(0), m_off_c0(0), m_off_c1(0),
          m_off_f1(0), m_off_f3(0), m_off_f8(0), m_off_f10(0),
          m_span(0),
          m_amp_ref(c_amp_nominal),
          m_psum_base(0),
          m_scan_pos(0),
          m_timebase(log, "MuseFrameReader", m_sample_rate, m_sample_rate / c_line_hz, MUSE_TOTAL_HEIGHT),
          m_tracking(false),
          m_last_positive(false),
          m_misses(0),
          m_anchored(false),
          m_seek_generation(0),
          m_frame_partial(true),
          m_line1_k(0),
          m_pending_drift(0),
          m_frame_start_offset(0),
          m_frame_period(0),
          m_frames_without_pulses(0),
          m_process_elapsed_ms(0),
          m_read_input_elapsed_ms(0),
          m_timed_frames(0),
          m_dump_line_count(0) {
    if (m_demodulate)
        m_demodulator = new MuseRfDemodulator(log, executable_dir, std::move(m_input), (float)sample_rate, vulkan_manager,
                                              benchmark_shaders, efm_enabled, efm_adaptive_filter_size);
}

bool MuseFrameReader::initialize(std::vector<std::unique_ptr<MuseInputBlock>> &buffers) {
    if (m_demodulator == nullptr) {
        m_input_reader = m_input.open(c_input_sub_buffer_size, &m_log);
        m_input_reader->initialize();
        // File mode: the reader hands out the file's own sample codes, so map them to
        // the ~0..255 range the sync levels are defined on from their width and
        // signedness (a 10-bit lds code 0..1023 becomes 0..255, a signed 16-bit one
        // -32768..32767 becomes 0..255 as well)
        const int bits = m_input_reader->bitsPerSample();
        m_input_scale = std::ldexp(1.f, 8 - bits);
        m_input_offset = m_input_reader->signedSamples() ? 128.f : 0.f;
        m_log.info(eInput, std::format("Input samples: {} bits, {}", bits,
                                       m_input_reader->signedSamples() ? "signed" : "unsigned"));
    } else {
        m_demodulator->initialize(MuseDemodulatedBlock::recommendedNumberOfBlockBuffers((float)m_sample_rate * MuseDemodulatedBlock::c_video_decimation_rate));
    }

    // The sync geometry, relative to pixel 1 of the line: plateau A over
    // pixels 2-4.5, plateau B over 7.5-10, the crossing searched between
    // pixels 4 and 7.5, the flatness probes at 1.5/3.5 and 7.5/9.5.  (A
    // plateau window or probe nearer pixel 1 reaches into the dip that
    // precedes the sync, which on the negative lines ends only at pixel 1.5:
    // chosen on the old DPLL's grid of the blue capture, where this passes
    // 99.9% of both polarities at the true position and 99% a sample
    // early, against 93% and 12% with the windows a half pixel earlier.)
    m_off_a0 = (int)lround(1.0 * m_pix);
    m_off_a1 = (int)lround(3.5 * m_pix);
    m_off_b0 = (int)lround(6.5 * m_pix);
    m_off_b1 = (int)lround(9.0 * m_pix);
    m_off_mid = (int)lround(5.0 * m_pix);
    m_off_c0 = (int)lround(4.0 * m_pix);
    m_off_c1 = (int)lround(7.5 * m_pix);
    m_off_f1 = (int)lround(1.5 * m_pix);
    m_off_f3 = (int)lround(3.5 * m_pix);
    m_off_f8 = (int)lround(7.5 * m_pix);
    m_off_f10 = (int)lround(9.5 * m_pix);
    m_span = m_off_b1 + 4;
    m_psum.assign(2 * c_input_sub_buffer_size + 1, 0.0);

    m_input_buffer = (float *)calloc(c_input_buffer_size, sizeof(float));
    m_input_dropout_buffer = (uint8_t *)calloc(c_input_buffer_size, 1);
    assert(m_input_buffer != nullptr && m_input_dropout_buffer != nullptr);

    return FrameReader::initialize(buffers);
}

// Idempotent: runs both from the explicit teardown path and from the destructor.
void MuseFrameReader::cleanup() {
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
    m_input_reader.reset();
}

// With the demodulator the seek only moves its input: the timebase is reset
// on the reader thread when the first block after the seek arrives
// (readInputBlock sees its generation change).  In file mode the input is
// moved here and the reset is requested, again for the reader thread.
bool MuseFrameReader::seek(double seconds) {
    if (m_input_is_realtime)
        return true;
    if (m_demodulator != nullptr) {
        if (!m_demodulator->seek(seconds))
            return false;
    } else {
        std::unique_lock<std::mutex> lock(m_mutex);
        const auto samples_to_seek = (int64_t)(seconds * m_sample_rate);
        m_log.info(eInput, std::format("Seeking relative time {} s, {} samples.", seconds, samples_to_seek));
        if (!m_input_reader->seek(samples_to_seek))
            return false;
        m_file_reset_request = true;
    }
    discardFilledBuffers();
    return true;
}

std::optional<uint32_t> MuseFrameReader::seekToInputSample(int64_t sample) {
    // The baseband path keeps no input offsets, so only the RF path can
    if (m_input_is_realtime || m_demodulator == nullptr)
        return std::nullopt;
    const auto generation = m_demodulator->seekToSample(sample);
    if (generation)
        discardFilledBuffers();
    return generation;
}

int64_t MuseFrameReader::inputSampleCount() const {
    return m_demodulator != nullptr ? m_demodulator->inputSampleCount() : -1;
}

void MuseFrameReader::setEfmEnabled(bool enabled) {
    if (m_demodulator != nullptr)
        m_demodulator->setEfmEnabled(enabled);
}

void MuseFrameReader::setEfmAdaptiveFilterSize(int size) {
    if (m_demodulator != nullptr)
        m_demodulator->setEfmAdaptiveFilterSize(size);
}

int MuseFrameReader::efmAdaptiveFilterSize() const {
    return m_demodulator != nullptr ? m_demodulator->efmAdaptiveFilterSize() : -1;
}

void MuseFrameReader::resetTimebase(const char *why) {
    m_timebase.reset();
    m_tracking = false;
    m_misses = 0;
    m_chains.clear();
    m_polarities.clear();
    m_anchored = false;
    m_pending_drift = 0;
    m_frame_partial = true;
    m_frames_without_pulses = 0;
    m_amp_ref = c_amp_nominal;
    m_scan_pos = (double)m_stream_pos;
    m_log.info(eInput, std::format("MuseFrameReader: timebase reset ({})", why));
}

void MuseFrameReader::threadFunc() {
    unique_ptr<MuseInputBlock> output_block = nullptr;

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
                m_log.info(eInput, "MuseFrameReader: stop requested");
                break;
            }
            output_block = std::move(m_vacant_input_buffers.front());
            m_vacant_input_buffers.pop_front();
            output_block->efm_data.clear();
        }

        if (!process(output_block)) {
            m_log.info(eInput, m_stop_request ? "MuseFrameReader: stop requested"
                                              : "MuseFrameReader: end of file");
            break;
        }

        output_block->input_offset = m_frame_start_offset;
        output_block->seek_generation = m_seek_generation;
        output_block->input_samples_per_muse_sample = m_frame_period / MUSE_TOTAL_WIDTH * m_input_samples_decimation_rate;
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv_filled.notify_one();
        m_filled_input_buffers.push_back(std::move(output_block));
        output_block = nullptr;
    }

    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv_filled.notify_one();
    m_reader_thread_finished = true;
}

bool MuseFrameReader::process(std::unique_ptr<MuseInputBlock> const &output_block) {
    auto t_start = std::chrono::steady_clock::now();
    for (;;) {
        if (m_stop_request)
            return false;
        if (consumeFinalized(output_block)) {
            m_process_elapsed_ms += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t_start).count();
            if (++m_timed_frames == c_timing_report_frames) {
                m_log.info(ePerformance, std::format(
                        "reader avg/frame (budget 33.33 ms): timebase+resample {:.2f} ms, input {:.2f} ms",
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

bool MuseFrameReader::readInputBlock(std::unique_ptr<MuseInputBlock> const &output_block) {
    // A stop has to be noticed here: process() only looks between frames,
    // and junk input never completes one.
    if (m_stop_request)
        return false;

    const int64_t B = (int64_t)c_input_sub_buffer_size;
    const int slot = (int)(m_blocks_fetched % c_number_of_input_sub_buffers);
    float *dst = m_input_buffer + (size_t)slot * c_input_sub_buffer_size;
    uint8_t *dst_do = m_input_dropout_buffer + (size_t)slot * c_input_sub_buffer_size;

    if (m_demodulator != nullptr) {
        auto block = m_demodulator->getNextDemodulatedBlock();
        if (block == nullptr) {
            m_log.info(eInput, "MuseFrameReader: no more demodulated blocks");
            return false;
        }
        if (block->seek_generation != m_seek_generation) {
            m_seek_generation = block->seek_generation;
            resetTimebase("seek");
        }
        memcpy(dst, block->video_data->data<float>(), c_input_sub_buffer_size * sizeof(float));
        memcpy(dst_do, block->dropouts->data<uint8_t>(), c_input_sub_buffer_size);
        m_sub_buffer_input_offsets[slot] = block->input_offset;
        if (output_block != nullptr && m_anchored)
            output_block->efm_data.insert(output_block->efm_data.end(), block->efm_data.begin(), block->efm_data.end());
        m_demodulator->returnBlock(block);
    } else {
        if (m_file_reset_request.exchange(false))
            resetTimebase("seek");
        const int got = m_input_reader->readFloats(dst);
        if (got != (int)c_input_sub_buffer_size) {
            if (!m_input_is_realtime) {
                m_log.info(eInput, "MuseFrameReader: end of file");
                return false;
            }
            return true; // nothing arrived yet; try again
        }
        if (m_input_scale != 1.f || m_input_offset != 0.f)
            for (size_t i = 0; i < c_input_sub_buffer_size; i++)
                dst[i] = dst[i] * m_input_scale + m_input_offset;
        memset(dst_do, 0, c_input_sub_buffer_size);
        m_sub_buffer_input_offsets[slot] = m_blocks_fetched * B;
    }

    // The slot just filled wrapped over data four blocks old; finalized
    // lines that still point there have waited too long (cannot happen in
    // normal flow, where lines are resampled one to two blocks after they
    // arrive)
    while (m_timebase.curveSize() > 0 && m_timebase.curve(0) < (double)((m_blocks_fetched - 3) * B)) {
        m_log.warn(eInput, std::format("MuseFrameReader: dropping stale line {} (resampling fell behind)",
                                       m_timebase.curveBase()));
        m_timebase.popLine();
    }

    // Debug aid: MUSELD_DUMP_DEMOD=<path> saves the blocks (float32 at the
    // stream rate, MUSE levels 0..255) for offline analysis -- the first 32
    // unless MUSELD_DUMP_DEMOD_BLOCKS says otherwise, and their dropout flags
    // to <path>.do.  Sample i of the dump is stream position i in the
    // timebase dump (see consumeFinalized), so the two must come from the
    // same run.
    static const char *dump_path = getenv("MUSELD_DUMP_DEMOD");
    static const bool dump_after_seek = getenv("MUSELD_DUMP_DEMOD_AFTER_SEEK") != nullptr;
    static int dump_blocks_left = -1;
    if (dump_path != nullptr && dump_blocks_left != 0 && (!dump_after_seek || m_seek_generation > 0)) {
        const bool first = dump_blocks_left == -1;
        if (first) {
            const char *blocks = getenv("MUSELD_DUMP_DEMOD_BLOCKS");
            dump_blocks_left = blocks != nullptr ? atoi(blocks) : 32;
            // MUSELD_DUMP_DEMOD_AFTER_SEEK=1 starts the dump at the first block after a
            // seek (the section --seek selects: the stream before it was decoded too);
            // <path>.pos holds the stream position of the dump's first sample for the tools
            if (FILE *f = fopen((std::string(dump_path) + ".pos").c_str(), "w")) {
                fprintf(f, "%lld\n", (long long)m_stream_pos);
                fclose(f);
            }
        }
        if (FILE *f = fopen(dump_path, first ? "wb" : "ab")) {
            fwrite(dst, sizeof(float), c_input_sub_buffer_size, f);
            fclose(f);
        }
        if (FILE *f = fopen((std::string(dump_path) + ".do").c_str(), first ? "wb" : "ab")) {
            fwrite(dst_do, 1, c_input_sub_buffer_size, f);
            fclose(f);
        }
        if (--dump_blocks_left == 0)
            m_log.info(eInput, std::format("Demodulated video dump complete: {}", dump_path));
    }

    m_blocks_fetched++;
    m_stream_pos += B;
    syncPass();

    // Signal-loss guard: with no usable sync edges for many lines the
    // lattice is stale; start over when they return
    if (m_tracking && (double)m_stream_pos - m_timebase.lastPulseTime() > 3000 * m_p_nominal)
        resetTimebase("no sync edges");
    return true;
}

// Prefix sums over the newest two blocks, then the acquisition or tracking
// pass over what they allow
void MuseFrameReader::syncPass() {
    const int64_t B = (int64_t)c_input_sub_buffer_size;
    m_psum_base = max<int64_t>(0, m_stream_pos - 2 * B);
    const int64_t n = m_stream_pos - m_psum_base;
    double acc = 0;
    m_psum[0] = 0;
    for (int64_t i = 0; i < n; i++) {
        acc += sample(m_psum_base + i);
        m_psum[i + 1] = acc;
    }
    if (m_tracking)
        track(m_stream_pos);
    else
        acquire(m_stream_pos);
}

// Is there an HD sync edge with its pixel 1 at stream position i?  Requires
// i - 2 >= m_psum_base and i + m_span < m_stream_pos.
bool MuseFrameReader::scoreCandidate(int64_t i, Candidate *c) const {
    auto mean = [this](int64_t a, int64_t b) {
        return (float)((m_psum[b - m_psum_base] - m_psum[a - m_psum_base]) / (double)(b - a));
    };
    const float a = mean(i + m_off_a0, i + m_off_a1);
    const float b = mean(i + m_off_b0, i + m_off_b1);
    const float d = b - a;
    const float ad = abs(d);
    if (ad < c_amp_min * m_amp_ref || ad > c_amp_max * m_amp_ref)
        return false;
    const float lo = min(a, b), hi = max(a, b);
    if (lo < c_level_lo_min || lo > c_level_lo_max || hi < c_level_hi_min || hi > c_level_hi_max)
        return false;
    const float m = sample(i + m_off_mid);
    if ((m - a) * d <= 0 || (b - m) * d <= 0)
        return false; // the middle sample is not between the plateaus
    if (abs(sample(i + m_off_f1) - sample(i + m_off_f3)) > c_flatness * ad
        || abs(sample(i + m_off_f8) - sample(i + m_off_f10)) > c_flatness * ad)
        return false; // a plateau is not flat: a ramp or picture detail
    c->i = i;
    c->a = a;
    c->b = b;
    c->positive = d > 0;
    c->t = 0;
    return true;
}

// The crossing of the plateau midpoint, through the cubic through the four
// samples around it (the same cubic the resampler uses)
bool MuseFrameReader::refineCrossing(Candidate *c) const {
    const float mid = 0.5f * (c->a + c->b);
    for (int64_t j = c->i + m_off_c0; j < c->i + m_off_c1; j++) {
        const float y0 = sample(j) - mid, y1 = sample(j + 1) - mid;
        const bool crosses = c->positive ? (y0 <= 0 && y1 > 0) : (y0 >= 0 && y1 < 0);
        if (!crosses)
            continue;
        const float b0 = sample(j - 1), b1 = sample(j), b2 = sample(j + 1), b3 = sample(j + 2);
        double lo = 0, hi = 1;
        for (int it = 0; it < 12; it++) {
            const double fm = 0.5 * (lo + hi);
            const float v = cubicInterpolate(b0, b1, b2, b3, (float)fm) - mid;
            ((v > 0) == c->positive ? hi : lo) = fm;
        }
        c->t = (double)j + 0.5 * (lo + hi);
        return true;
    }
    return false;
}

// Acquisition: every line period of the stream is scanned for edge
// candidates, and candidates on consecutive lines with alternating polarity
// are chained.  A chain of c_chain_lock lines is the lattice; the chains a
// picture's vertical edges start die within a few lines, since they keep
// their direction.
void MuseFrameReader::acquire(int64_t stream_end) {
    const double tol = 4 * m_pix;
    while (m_scan_pos + m_p_nominal + m_span + 2 < (double)stream_end) {
        const int64_t w0 = max<int64_t>((int64_t)m_scan_pos, m_psum_base + 2);
        const int64_t w1 = (int64_t)(m_scan_pos + m_p_nominal);
        m_cands.clear();
        // runs of passing positions of one polarity are one edge: keep the
        // position with the largest amplitude
        Candidate best{};
        float best_ad = 0;
        bool in_run = false;
        for (int64_t i = w0; i < w1; i++) {
            Candidate c;
            const bool ok = scoreCandidate(i, &c);
            if (ok && in_run && c.positive == best.positive) {
                if (abs(c.b - c.a) > best_ad) {
                    best = c;
                    best_ad = abs(c.b - c.a);
                }
            } else {
                if (in_run)
                    m_cands.push_back(best);
                in_run = ok;
                if (ok) {
                    best = c;
                    best_ad = abs(c.b - c.a);
                }
            }
        }
        if (in_run)
            m_cands.push_back(best);
        for (size_t i = 0; i < m_cands.size();) {
            if (refineCrossing(&m_cands[i]))
                i++;
            else
                m_cands.erase(m_cands.begin() + (ptrdiff_t)i);
        }

        vector<bool> used(m_cands.size(), false);
        for (size_t ci = 0; ci < m_chains.size();) {
            Chain &c = m_chains[ci];
            double expected = c.t_last + c.p * (c.misses + 1);
            // a line expected before this window went unmatched there
            while (expected < (double)w0 - tol && c.misses <= c_chain_misses) {
                c.misses++;
                expected = c.t_last + c.p * (c.misses + 1);
            }
            if (c.misses > c_chain_misses) {
                m_chains.erase(m_chains.begin() + (ptrdiff_t)ci);
                continue;
            }
            if (expected >= (double)w1 + tol) {
                ci++;
                continue; // next window
            }
            const double tol_here = tol * (c.misses + 1);
            int pick = -1;
            double dist_best = tol_here;
            for (size_t j = 0; j < m_cands.size(); j++) {
                if (used[j])
                    continue;
                const double dist = abs(m_cands[j].t - expected);
                if (dist < dist_best) { dist_best = dist; pick = (int)j; }
            }
            if (pick < 0) {
                if (++c.misses > c_chain_misses) {
                    static const bool trace = getenv("MUSELD_TRACE_SYNC") != nullptr;
                    if (trace && c.pulses.size() >= 8)
                        m_log.debug(eInput, std::format("acquire: chain of {} lines died at stream position {:.0f} (line {} of it)",
                                                        c.pulses.size(), expected, c.k_last + c.misses));
                    m_chains.erase(m_chains.begin() + (ptrdiff_t)ci);
                    continue;
                }
                ci++;
                continue;
            }
            const Candidate &cand = m_cands[(size_t)pick];
            used[(size_t)pick] = true;
            if (c.pulses.size() >= 2)
                c.p += 0.2 * ((cand.t - c.t_last) / (c.misses + 1) - c.p);
            else
                c.p = (cand.t - c.t_last) / (c.misses + 1);
            c.k_last += c.misses + 1;
            c.t_last = cand.t;
            c.misses = 0;
            c.pulses.push_back(cand.t);
            c.ks.push_back(c.k_last);
            c.pols.push_back(cand.positive);
            c.amps.push_back(abs(cand.b - cand.a));
            // The polarity sequence tells the sync from a picture edge, which
            // keeps its direction: a chain whose consecutive lines mostly do
            // not alternate dies as soon as that is clear.  (MUSE's
            // line-sequential chroma flips a static colour edge line to
            // line, so alternation alone is not proof: the frame pulses are.)
            int steps = 0, flips = 0;
            for (size_t n = 1; n < c.pulses.size(); n++)
                if (c.ks[n] == c.ks[n - 1] + 1) {
                    steps++;
                    flips += c.pols[n] != c.pols[n - 1];
                }
            if (steps >= c_chain_alternation_check && flips * 10 < steps * 6) {
                m_chains.erase(m_chains.begin() + (ptrdiff_t)ci);
                continue;
            }
            // A chain long enough is verified by the frame pulses at every
            // frame start its polarities show (+ - - + on consecutive lines:
            // lines 1-4), and becomes the lattice at the first one that holds
            if (c.pulses.size() >= (size_t)c_chain_lock) {
                bool locked = false;
                for (size_t i = c.checked; i + 3 < c.pulses.size(); i++) {
                    if (c.ks[i + 3] != c.ks[i] + 3 || !c.pols[i] || c.pols[i + 1] || c.pols[i + 2] || !c.pols[i + 3])
                        continue;
                    const double step = c.p / MUSE_TOTAL_WIDTH;
                    const double diff = framePulseSum(c.pulses[i], step) - framePulseSum(c.pulses[i + 1], step);
                    float amp = 0;
                    for (float a : c.amps)
                        amp += a;
                    amp /= (float)c.amps.size();
                    if (diff >= 70.0 * amp) {
                        locked = lockOnChain(c, i, diff);
                        break;
                    }
                    m_log.debug(eInput, std::format("MuseFrameReader: chain of {} lines: no frame pulses at its line {}, difference {:.0f}",
                                                    c.pulses.size(), c.ks[i], diff));
                }
                if (locked) {
                    track(stream_end);
                    return;
                }
                c.checked = c.pulses.size() - 3;
            }
            if (c.pulses.size() > (size_t)c_chain_max) {
                m_chains.erase(m_chains.begin() + (ptrdiff_t)ci);
                continue;
            }
            ci++;
        }
        for (size_t j = 0; j < m_cands.size(); j++) {
            if (used[j] || m_chains.size() >= c_max_chains)
                continue;
            m_chains.push_back(Chain{m_cands[j].t, m_p_nominal, 0, 0,
                                     {m_cands[j].t}, {0}, {m_cands[j].positive}, {abs(m_cands[j].b - m_cands[j].a)}, 0});
        }
        m_scan_pos += m_p_nominal;
    }
}

// The frame pulses of lines 1 and 2 (pixels 313-478): line 1 carries the
// pattern, line 2 its inverse, so the two sums differ by about 166 * 223 *
// 0.73 on a correctly scaled signal and by a few thousand on anything else
double MuseFrameReader::framePulseSum(double t, double step) const {
    double sum = 0;
    for (int pixel = 313; pixel <= 478; pixel++) {
        const int pulse_pixel = pixel - 313;
        const float value = (pulse_pixel < 144 ? pulse_pixel / 4 % 2 == 0 : pulse_pixel < 160) ? 1.f : -1.f;
        const double pos = t + (pixel - 6) * step;
        const auto ip = (int64_t)pos;
        sum += value * cubicInterpolate(sample(ip - 1), sample(ip), sample(ip + 1), sample(ip + 2),
                                        (float)(pos - (double)ip));
    }
    return sum;
}

// A verified chain becomes the lattice: its pulses in order, the frame
// anchored at the line 1 the frame pulses confirmed
bool MuseFrameReader::lockOnChain(const Chain &c, size_t line1_ix, double diff) {
    m_timebase.reset();
    m_polarities.clear();
    int64_t line1_k = 0;
    for (size_t n = 0; n < c.pulses.size(); n++) {
        const int64_t k = m_timebase.addPulse(c.pulses[n]);
        if (k >= 0)
            m_polarities.emplace_back(k, c.pols[n]);
        if (n == line1_ix)
            line1_k = k;
    }
    m_timebase.setPeriod(c.p);
    float amp = 0;
    for (float a : c.amps)
        amp += a;
    m_amp_ref = amp / (float)c.amps.size();
    m_last_positive = c.pols.back();
    m_misses = 0;
    m_tracking = true;
    m_line1_k = line1_k;
    m_anchored = true;
    m_pending_drift = 0;
    m_frame_partial = true;
    m_log.info(eInput, std::format("MuseFrameReader: lattice acquired from a chain of {} lines at stream position {:.0f}, "
                                   "line period {:.3f} samples ({:+.0f} ppm), sync amplitude {:.0f}, "
                                   "frame pulse difference {:.0f}; frame starts at lattice line {}",
                                   c.pulses.size(), c.t_last, c.p, (c.p / m_p_nominal - 1) * 1e6, m_amp_ref, diff, m_line1_k));
    if (m_log.isEnabled(eDebug, eInput)) {
        std::string intervals;
        for (size_t n = 1; n < c.pulses.size(); n++)
            intervals += std::format(" {:+.1f}/{}{:.0f}", c.pulses[n] - c.pulses[n - 1] - m_p_nominal, c.pols[n] ? '+' : '-', c.amps[n]);
        m_log.debug(eInput, "MuseFrameReader: chain intervals minus nominal / polarity and amplitude:" + intervals);
    }
    m_chains.clear();
    return true;
}

// Tracking: each line is looked for where the lattice predicts it, in a
// window that widens with the gap since the last edge, with the polarity the
// alternation says -- the other polarity is accepted when it is all there is
// (the lines 2-3 break, or a line after a missed one).
void MuseFrameReader::track(int64_t stream_end) {
    for (;;) {
        const int64_t k0 = m_timebase.lastPulseLine();
        const int64_t k = k0 + 1 + m_misses;
        // Predicted from the last edge by the chain's period: good to a
        // sample for the next line and to a few over a gap of tens of lines.
        // (The smoother's look-ahead head, extrapolated up to 64 lines, is
        // good to a few pixels only, which is enough to detect a slipped
        // line but not to place a +-4 pixel window: used for the window it
        // missed 15% of the lines on a clean capture.)
        const double gap = (double)(k - k0) * m_timebase.period();
        const double pred = m_timebase.lastPulseTime() + gap;
        const double w = min(4 * m_pix + 0.0005 * gap, 0.4 * m_p_nominal);
        // pred is the crossing (pixel 6); the candidates are pixel 1 positions
        const double pred_pixel1 = pred - m_off_mid;
        const int64_t lo = max<int64_t>((int64_t)floor(pred_pixel1 - w), m_psum_base + 2);
        const int64_t hi = (int64_t)ceil(pred_pixel1 + w);
        if (hi + m_span + 2 >= stream_end)
            return; // the line's samples have not all arrived
        const bool expect_positive = m_last_positive != (((k - k0) & 1) != 0);
        Candidate alt{}, same{};
        bool have_alt = false, have_same = false;
        double dist_alt = 1e30, dist_same = 1e30;
        for (int64_t i = lo; i <= hi; i++) {
            Candidate c;
            if (!scoreCandidate(i, &c))
                continue;
            const double dist = abs((double)i - pred_pixel1);
            if (c.positive == expect_positive) {
                if (dist < dist_alt) { dist_alt = dist; alt = c; have_alt = true; }
            } else if (dist < dist_same) {
                dist_same = dist; same = c; have_same = true;
            }
        }
        Candidate *chosen = have_alt ? &alt : have_same ? &same : nullptr;
        static const bool trace = getenv("MUSELD_TRACE_SYNC") != nullptr;
        const bool refined = chosen != nullptr && refineCrossing(chosen);
        if (refined) {
            const int64_t kk = m_timebase.addPulse(chosen->t);
            if (trace && kk < 0)
                m_log.debug(eInput, std::format("track: line {} pred {:.1f} candidate at {:.1f} dropped by addPulse (last {} at {:.1f}, p {:.2f})",
                                                k, pred, chosen->t, k0, m_timebase.lastPulseTime(), m_timebase.period()));
            if (kk >= 0) {
                m_polarities.emplace_back(kk, chosen->positive);
                if (m_polarities.size() > 4096)
                    m_polarities.pop_front();
                m_last_positive = chosen->positive;
                m_misses = 0;
                m_amp_ref += 0.01f * (abs(chosen->b - chosen->a) - m_amp_ref);
                continue;
            }
        }
        if (trace) {
            int n_pass = 0;
            for (int64_t i = lo; i <= hi; i++) {
                Candidate c;
                n_pass += scoreCandidate(i, &c);
            }
            m_log.debug(eInput, std::format("track: line {} missed: pred {:.1f} window [{}, {}] passing {} alt {} same {} refine_ok {}; "
                                            "samples at pred-2..+20: {}",
                                            k, pred, lo, hi, n_pass, have_alt, have_same,
                                            refined, [&] {
                                                std::string v;
                                                for (int64_t i = (int64_t)pred - 2; i < (int64_t)pred + 20; i++)
                                                    v += std::format("{:.0f} ", sample(i));
                                                return v;
                                            }()));
        }
        m_misses++;
    }
}

// The vertical anchor: two consecutive negative syncs are lines 2 and 3.
// Required with the positive syncs of lines 1 and 4 around them, all four
// on consecutive lines: a misread edge gives a same-polarity pair too, but
// not one that sits in an otherwise intact alternation.
void MuseFrameReader::evaluateAnchors() {
    while (m_polarities.size() >= 4) {
        const auto [k1, p1] = m_polarities[0];
        const auto [k2, p2] = m_polarities[1];
        const auto [k3, p3] = m_polarities[2];
        const auto [k4, p4] = m_polarities[3];
        m_polarities.pop_front();
        if (!p1 || p2 || p3 || !p4 || k2 != k1 + 1 || k3 != k2 + 1 || k4 != k3 + 1)
            continue;
        const int64_t cand = k1; // line 1
        if (!m_anchored) {
            m_line1_k = cand;
            while (m_line1_k < m_timebase.curveBase())
                m_line1_k += MUSE_TOTAL_HEIGHT;
            m_anchored = true;
            m_pending_drift = 0;
            m_log.info(eInput, std::format("MuseFrameReader: anchored, frame starts at lattice line {}", m_line1_k));
        } else {
            int64_t drift = (cand - m_line1_k) % MUSE_TOTAL_HEIGHT;
            if (drift > MUSE_TOTAL_HEIGHT / 2)
                drift -= MUSE_TOTAL_HEIGHT;
            if (drift < -MUSE_TOTAL_HEIGHT / 2)
                drift += MUSE_TOTAL_HEIGHT;
            if (drift == 0) {
                m_pending_drift = 0;
            } else if (drift != m_pending_drift) {
                // require two consecutive frames to agree before re-anchoring:
                // a single odd pair is a misread edge, a real slip confirms next frame
                m_pending_drift = drift;
            } else {
                m_log.warn(eInput, std::format("MuseFrameReader: vertical drift {} lines, re-anchoring", drift));
                m_line1_k = cand;
                while (m_line1_k < m_timebase.curveBase())
                    m_line1_k += MUSE_TOTAL_HEIGHT;
                m_pending_drift = 0;
            }
        }
    }
}

int64_t MuseFrameReader::inputOffsetOfStreamPos(double stream_pos) const {
    const int64_t B = (int64_t)c_input_sub_buffer_size;
    const int64_t p = (int64_t)stream_pos;
    const int slot = (int)((p / B) % c_number_of_input_sub_buffers);
    return m_sub_buffer_input_offsets[slot] + (p % B) * m_input_samples_decimation_rate;
}

// The frame pulses on lines 1 and 2 (pixels 313-478): the correlation the
// old DPLL reader locked on, here the check that the anchor is right
bool MuseFrameReader::framePulsesPresent(std::unique_ptr<MuseInputBlock> const &output_block) const {
    const float *rows = output_block->video_data->data<float>();
    double sum[2] = {0, 0};
    for (int row = 0; row < 2; row++)
        for (int pixel = 313; pixel <= 478; pixel++) {
            const int pulse_pixel = pixel - 313;
            const float value = (pulse_pixel < 144 ? pulse_pixel / 4 % 2 == 0 : pulse_pixel < 160) ? 1.f : -1.f;
            sum[row] += value * rows[row * MUSE_TOTAL_WIDTH + pixel - 1];
        }
    // 166 pixels of full-swing pattern give about 166 * 223 * 0.73 on a
    // correctly scaled signal, random content a few thousand: threshold
    // halfway, scaled by the sync amplitude (121 nominal)
    return sum[0] - sum[1] > 70.0 * m_amp_ref;
}

bool MuseFrameReader::consumeFinalized(std::unique_ptr<MuseInputBlock> const &output_block) {
    const int64_t B = (int64_t)c_input_sub_buffer_size;
    static const char *dump_prefix = getenv("MUSELD_DUMP_TIMEBASE");
    while (m_timebase.curveSize() >= 2) {
        const double t0 = m_timebase.curve(0), t1 = m_timebase.curve(1);
        if (t1 + 3 >= (double)m_stream_pos)
            return false; // resampling needs samples that have not arrived yet
        const double min_valid = (double)((m_blocks_fetched - c_number_of_input_sub_buffers) * B + 8 * m_pix);
        bool resampled_last = false;
        if (t0 >= min_valid && m_anchored) {
            if (m_timebase.curveBase() - m_line1_k + 1 > MUSE_TOTAL_HEIGHT)
                m_line1_k += MUSE_TOTAL_HEIGHT;
            const int64_t muse_line = m_timebase.curveBase() - m_line1_k + 1;
            if (muse_line >= 1 && muse_line <= MUSE_TOTAL_HEIGHT) {
                const double step = (t1 - t0) / MUSE_TOTAL_WIDTH;
                if (muse_line == 1) {
                    m_frame_start_offset = inputOffsetOfStreamPos(t0 - 5 * step);
                    m_frame_period = t1 - t0;
                    if (m_frame_partial) {
                        // The EFM data gathered since the anchoring belongs
                        // to the partial frame dropped here, not to this one
                        output_block->efm_data.clear();
                    }
                    m_frame_partial = false;
                }
                resampleLine(output_block, (int)muse_line, t0, t1);
                // Debug aid: MUSELD_DUMP_TIMEBASE=<prefix> appends one record per line to
                // <prefix>.muse.f64 (the format tools/muse-sync-delta.py reads):
                // the line count, the MUSE line number, the stream position of pixel 1, a
                // NaN where the loop error was, the samples per pixel, and 1 for "locked"
                if (dump_prefix != nullptr) {
                    if (FILE *f = fopen((std::string(dump_prefix) + ".muse.f64").c_str(), "ab")) {
                        const double rec[6] = {(double)m_dump_line_count, (double)muse_line, t0 - 5 * step,
                                               std::nan(""), step, 1.0};
                        fwrite(rec, sizeof rec, 1, f);
                        fclose(f);
                    }
                    m_dump_line_count++;
                }
                if (muse_line == 2 && !m_frame_partial) {
                    if (framePulsesPresent(output_block)) {
                        m_frames_without_pulses = 0;
                    } else if (++m_frames_without_pulses >= c_frames_without_pulses) {
                        m_log.warn(eInput, std::format("MuseFrameReader: no frame pulses on lines 1-2 for {} frames",
                                                       m_frames_without_pulses));
                        resetTimebase("frame pulses not found");
                        return false;
                    }
                }
                resampled_last = muse_line == MUSE_TOTAL_HEIGHT;
            }
        }
        m_timebase.popLine();
        // A frame whose first rows predate the anchoring would show half of
        // one picture and half of another: wait for the next whole frame
        if (resampled_last && !m_frame_partial)
            return true;
    }
    return false;
}

// Pixel 6 sits on the crossing at t0; the line's 480 pixels run from five
// pixels before it to the next line's crossing
void MuseFrameReader::resampleLine(std::unique_ptr<MuseInputBlock> const &output_block,
                                   int row, double t0, double t1) {
    float *out = output_block->video_data->data<float>() + (size_t)MUSE_TOTAL_WIDTH * (row - 1);
    uint8_t *out_do = output_block->dropout_data->data<uint8_t>() + (size_t)MUSE_TOTAL_WIDTH * (row - 1);
    const double step = (t1 - t0) / MUSE_TOTAL_WIDTH;
    double pos = t0 - 5 * step;
    for (int j = 0; j < MUSE_TOTAL_WIDTH; j++, pos += step) {
        const auto ip = (int64_t)pos;
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
