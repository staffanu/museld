// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_MUSEFRAMEREADER_H
#define MUSELD_MUSEFRAMEREADER_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>
#include "FrameReader.h"
#include "MuseRfDemodulator.h"
#include "input/InputReader.h"
#include "util/ConstExprHelpers.h"
#include "MuseInputBlock.h"
#include "timebase/LineTimebase.h"

// The MUSE reader with the feed-forward timebase (LineTimebase, shared with
// the SDTV readers): the HD sync edge at the start of every line is measured
// on the demodulated stream, the measurements are chained onto the line
// lattice and smoothed, and each line is then resampled to its 480 pixels
// between two curve points.  It replaced (2026-10) a causal per-line DPLL,
// which detected the edge on its own resampled output and so could not
// vectorize the resampling, chased the 4 ns single-edge noise with its
// proportional term, and acquired lock by trying one phase at a time.
//
// The HD sync (pixels 1-12 of the line) is a plateau over pixels 1-5, an
// edge between pixels 5 and 7 and the opposite plateau over pixels 8-10, at
// fixed levels about 121 apart on the 0..255 scale.  It is positive (low
// then high) on line 1 and the even lines from 4, negative on the others, so
// the polarity alternates line to line except at lines 2-3, both negative.
// A picture edge of the same size keeps its direction from line to line;
// the alternation is what tells the sync from the picture, and the one
// break per frame is the vertical anchor.  A line's measurement is the
// crossing of the midpoint between its two plateau means, through a cubic
// through the samples: on three 62.5 MHz captures its noise is 3.4-4.2 ns
// rms (tools/muse-sync-delta.py).  Pixel 6 is at the crossing, as it was on
// the old DPLL's grid, so the decoder sees the same sampling phase.
class MuseFrameReader : public FrameReader<MuseInputBlock> {
public:
    explicit MuseFrameReader(Logger &log, const std::string &executable_dir, musevk::VulkanManager &vulkan_manager,
                             PreparedInput input,
                             double sample_rate, double initial_seek_seconds,
                             bool demodulate, bool benchmark_shaders, bool efm_enabled,
                             int efm_adaptive_filter_size,
                             const std::optional<std::string> &output_filename);
    MuseFrameReader(const MuseFrameReader&) = delete;
    void operator=(const MuseFrameReader&) = delete;
    // Join the reader thread while threadFunc() and the demodulator still
    // exist; the base destructor's cleanup() would be too late.
    ~MuseFrameReader() override {
        MuseFrameReader::cleanup();
    }

    bool initialize(std::vector<std::unique_ptr<MuseInputBlock>> &buffers) override;
    void cleanup() override;
    bool seek(double seconds) override;
    std::optional<uint32_t> seekToInputSample(int64_t sample) override;
    [[nodiscard]] double inputSampleRate() const override {
        return m_demodulator != nullptr ? m_sample_rate * MuseDemodulatedBlock::c_video_decimation_rate : 0;
    }
    [[nodiscard]] int64_t inputSampleCount() const override;
    void setEfmEnabled(bool enabled) override;
    void setEfmAdaptiveFilterSize(int size) override;
    [[nodiscard]] int efmAdaptiveFilterSize() const override;

protected:
    void threadFunc() override;

private:
    // A sync edge candidate: the stream position of its pixel 1 (an integer
    // sample), the two plateau means and the refined crossing
    struct Candidate {
        int64_t i;
        float a, b;         // plateau means, pixels 1.5-4.5 and 7.5-10
        bool positive;
        double t;           // the crossing, in stream samples
    };
    // An acquisition chain: candidates a line period apart, whatever their
    // polarity -- the polarity sequence is judged afterwards
    struct Chain {
        double t_last;
        double p;           // its own line period, from its intervals
        int misses;
        int64_t k_last;     // line of the last candidate, counted from the chain's first
        std::vector<double> pulses;
        std::vector<int64_t> ks;
        std::vector<bool> pols;
        std::vector<float> amps;
        size_t checked;     // pulses before this index were examined for a frame start
    };

    // Fetch one block into the ring and run the sync pass on it; appends the
    // EFM data to output_block while anchored
    bool readInputBlock(std::unique_ptr<MuseInputBlock> const &output_block);
    void syncPass();
    void acquire(int64_t stream_end);
    void track(int64_t stream_end);
    bool scoreCandidate(int64_t i, Candidate *c) const;
    bool refineCrossing(Candidate *c) const;
    void evaluateAnchors();
    // Resample finalized lines into the frame; returns true when a frame completed
    bool consumeFinalized(std::unique_ptr<MuseInputBlock> const &output_block);
    void resampleLine(std::unique_ptr<MuseInputBlock> const &output_block, int row, double t0, double t1);
    bool framePulsesPresent(std::unique_ptr<MuseInputBlock> const &output_block) const;
    // The frame pulse correlation of the line whose sync crossing is at t, on the ring
    double framePulseSum(double t, double step) const;
    bool lockOnChain(const Chain &c, size_t line1_ix, double diff);
    void resetTimebase(const char *why);
    [[nodiscard]] bool process(std::unique_ptr<MuseInputBlock> const &output_block);
    int64_t inputOffsetOfStreamPos(double stream_pos) const;
    [[nodiscard]] float sample(int64_t p) const { return m_input_buffer[(size_t)p & c_input_buffer_size_mask]; }

    PreparedInput m_input; // until initialize() opens it (file mode) or the demodulator takes it
    bool m_demodulate;
    std::unique_ptr<InputReader> m_input_reader;
    double m_sample_rate;               // of the stream the sync pass sees (demodulated, video-decimated)
    int m_input_samples_decimation_rate;
    MuseRfDemodulator *m_demodulator;
    // File mode: the reader hands out the file's own sample codes, mapped to
    // the ~0..255 MUSE range the sync levels are defined on
    float m_input_scale;
    float m_input_offset;
    std::atomic<bool> m_file_reset_request; // a seek in file mode, acted on by the reader thread

    // The input ring holds the last four blocks: the smoother's look-ahead
    // lag (192 lines) is under a block at 31.25 MHz, so a line is resampled
    // one to two blocks after its samples arrived.  Power of two for cheap
    // masking.
    static constexpr int c_number_of_input_sub_buffers = 4;
    static constexpr size_t c_input_sub_buffer_size = MuseDemodulatedBlock::c_video_block_size;
    static constexpr size_t c_input_buffer_size = c_input_sub_buffer_size * c_number_of_input_sub_buffers;
    static_assert((c_input_sub_buffer_size & (c_input_sub_buffer_size - 1)) == 0);
    static_assert((c_number_of_input_sub_buffers & (c_number_of_input_sub_buffers - 1)) == 0);
    static constexpr size_t c_input_buffer_size_mask = c_input_buffer_size - 1;

    float *m_input_buffer;
    uint8_t *m_input_dropout_buffer;
    int64_t m_sub_buffer_input_offsets[c_number_of_input_sub_buffers];
    int64_t m_blocks_fetched;
    int64_t m_stream_pos;               // samples fetched so far

    // --- sync geometry, in stream samples ---
    double m_pix;                       // samples per MUSE pixel
    double m_p_nominal;                 // samples per line
    int m_off_a0, m_off_a1;             // plateau A window, relative to pixel 1
    int m_off_b0, m_off_b1;             // plateau B window
    int m_off_mid;                      // pixel 6
    int m_off_c0, m_off_c1;             // where the crossing is looked for
    int m_off_f1, m_off_f3, m_off_f8, m_off_f10; // flatness probes on the plateaus
    int m_span;                         // samples needed after pixel 1 to score a candidate
    float m_amp_ref;                    // tracked sync amplitude (plateau difference)
    std::vector<double> m_psum;         // prefix sums over the stretch being scanned
    int64_t m_psum_base;

    // --- acquisition ---
    double m_scan_pos;                  // next stream position the acquisition pass examines
    std::vector<Chain> m_chains;
    std::vector<Candidate> m_cands;

    // --- tracking ---
    LineTimebase m_timebase;
    bool m_tracking;
    bool m_last_positive;               // polarity of the last accepted pulse
    int m_misses;                       // lines since it without a usable edge
    std::deque<std::pair<int64_t, bool>> m_polarities; // (lattice line, positive) of accepted pulses

    // --- vertical anchor: MUSE line numbers on the lattice ---
    bool m_anchored;
    uint32_t m_seek_generation;
    bool m_frame_partial;
    int64_t m_line1_k;
    int64_t m_pending_drift;
    int64_t m_frame_start_offset;
    double m_frame_period;
    int m_frames_without_pulses;        // consecutive frames whose lines 1-2 lack the frame pulses

    static constexpr int c_timing_report_frames = 128;
    double m_process_elapsed_ms;
    double m_read_input_elapsed_ms;
    int m_timed_frames;
    int64_t m_dump_line_count;
};

#endif //MUSELD_MUSEFRAMEREADER_H
