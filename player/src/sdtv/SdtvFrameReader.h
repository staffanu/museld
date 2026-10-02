// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_SDTVFRAMEREADER_H
#define MUSECPP_SDTVFRAMEREADER_H

#include <cstdint>
#include <deque>
#include <vector>
#include "FrameReader.h"
#include "SdtvRfDemodulator.h"
#include "input/InputReader.h"
#include "util/ConstExprHelpers.h"
#include "SdtvInputBlock.h"

// Feed-forward timebase: instead of a causal DPLL resampling the signal as it
// arrives, the reader finds sync pulses in a cheap lowpassed/decimated pass,
// assigns them to an integer line lattice, fits a smooth curve T(k) -- the
// input position of the start of line k -- through them with a fixed-lag
// Kalman smoother, and only then resamples each line between its two curve
// points.  Look-ahead makes disc wow trivial to follow (the white squall test
// disc swings its line period +-0.24% once per revolution, faster at the
// extremes than any per-line servo can slew), missing and false pulses are
// handled in the smoother (an outlier is rejected unless the next pulse
// corroborates it), and since every output sample's
// input position is known before resampling, the resampling itself has no
// feedback and can move to SIMD or the GPU wholesale.
class SdtvFrameReader : public FrameReader<SdtvInputBlock> {
public:
    explicit SdtvFrameReader(Logger &log, const std::string &executable_dir, musevk::VulkanManager &vulkan_manager,
                             const std::string &filename, InputFormat input_format,
                             double sample_rate, double initial_seek_seconds,
                             bool benchmark_shaders, AudioTrack audio_track, int efm_adaptive_filter_size,
                             const std::optional<std::string> &output_filename,
                             const VideoStandard &video_standard);
    SdtvFrameReader(const SdtvFrameReader&) = delete;
    void operator=(const SdtvFrameReader&) = delete;
    // Join the reader thread while threadFunc() and the demodulator still
    // exist; the base destructor's cleanup() would be too late.
    ~SdtvFrameReader() override {
        SdtvFrameReader::cleanup();
    }

    bool initialize(std::vector<std::unique_ptr<SdtvInputBlock>> &buffers) override;
    void cleanup() override;
    void seek(double seconds) override;
    void setAudioTrack(AudioTrack track) override;
    void setAnalogCx(bool enabled) override;
    void setEfmAdaptiveFilterSize(int size) override;
    [[nodiscard]] int efmAdaptiveFilterSize() const override;

protected:
    void threadFunc() override;

private:
    // Fetch one demodulated block into the ring and run the sync pass on it;
    // appends the audio side data to output_block while anchored
    bool readInputBlock(std::unique_ptr<SdtvInputBlock> const &output_block);
    void syncPass(const float *data, int64_t stream_base, int count);
    void handlePulse(double t, double width_us);
    bool canFinalize() const;
    void finalizeBatch();
    void evaluateAnchors();
    // Resample finalized lines into the frame; returns true when a frame completed
    bool consumeFinalized(std::unique_ptr<SdtvInputBlock> const &output_block);
    void resampleLine(std::unique_ptr<SdtvInputBlock> const &output_block, int row, double t0, double t1);
    void resetTimebase(const char *why);

    [[nodiscard]] bool process(std::unique_ptr<SdtvInputBlock> const &output_block);

    int64_t inputOffsetOfStreamPos(double stream_pos) const;

    const VideoStandard &m_video_standard;
    SdtvRfDemodulator *m_demodulator;
    double m_sample_rate;               // demodulated (video-decimated) rate
    int m_input_samples_decimation_rate;
    double m_p_nominal;                 // demodulated samples per line

    // The input ring holds the last four demodulated blocks: the smoother's
    // look-ahead lag is about one block, so a line is resampled one to two
    // blocks after its samples arrived and the ring keeps a comfortable
    // margin.  The total size stays a power of two for cheap masking.
    static constexpr int c_number_of_input_sub_buffers = 4;
    static constexpr size_t c_input_sub_buffer_size = SdtvRfDemodulatorConstants::c_video_block_size;
    static constexpr size_t c_input_buffer_size = c_input_sub_buffer_size * c_number_of_input_sub_buffers;
    static_assert((c_input_sub_buffer_size & (c_input_sub_buffer_size - 1)) == 0);
    static_assert((c_number_of_input_sub_buffers & (c_number_of_input_sub_buffers - 1)) == 0);
    static constexpr size_t c_input_buffer_size_mask = c_input_buffer_size - 1;

    float *m_input_buffer;
    uint8_t *m_input_dropout_buffer;
    int64_t m_sub_buffer_input_offsets[c_number_of_input_sub_buffers];
    int64_t m_blocks_fetched;
    int64_t m_stream_pos;               // demodulated samples fetched so far

    // --- cheap sync pass: boxcar-decimated, lowpassed pulse detection ---
    // (this is the part that later becomes a small GPU side buffer)
    static constexpr int c_sync_decim = 8;
    static constexpr int c_sync_filt_ring = 512; // decimated samples of filtered history (> 1 line)
    std::vector<float> m_sync_fir;      // lowpass at the decimated rate
    std::vector<float> m_sync_fir_in;   // FIR input ring (boxcar outputs)
    int m_sync_boxcar_phase;
    int64_t m_sync_dec_count;           // decimated samples produced
    float m_sync_filt[c_sync_filt_ring];
    double m_sync_delay;                // group delay, in demodulated samples
    bool m_sync_below;                  // hysteresis slicer state
    int64_t m_sync_fall_idx;            // decimated index of the pending fall
    int64_t m_sync_rise_idx;            // decimated index of the previous rise
    float m_blank_level;                // slow average of the back-porch level

    // --- lattice: integer line numbers for hsync timestamps ---
    // Chained pulse to pulse: the nominal period is known a priori to ~0.1%
    // (wow included), so round(dt / period) is unambiguous for gaps of
    // hundreds of lines and needs no lock-in phase.  The period is tracked
    // from clean consecutive intervals only, never from the filter (see
    // handlePulse for the runaway that causes).  Line numbers are
    // attached separately by the vertical anchor below.
    bool m_lattice_valid;
    double m_lat_t;                     // last accepted pulse
    double m_p_run;                     // slowly adapted local period
    int64_t m_last_meas_k;              // its line
    // Integer-line slip correction against the curve: a shift confirmed by
    // this many consecutive pulses re-labels the chain
    int64_t m_slip_shift;
    int64_t m_slip_first_k;
    int m_slip_count;
    // Running measurement-noise estimate: mean |second difference| of
    // consecutive-line pulse times (see handlePulse)
    double m_last_dt;
    int64_t m_last_dk;
    double m_d2_mean;
    std::deque<std::pair<int64_t, double>> m_meas; // (k, t) hsync measurements
    std::deque<double> m_broad_falls;   // broad (vertical sync) pulse falls

    // --- fixed-lag Kalman smoother over [position, period] ---
    struct KalState {
        double t, p;                    // position of line start, period
        double P00, P01, P11;           // symmetric covariance
    };
    static constexpr int c_kal_batch = 64;  // lines finalized per batch
    static constexpr int c_kal_lag = 128;   // look-ahead beyond the batch
    bool m_kal_valid;
    KalState m_kal;                     // filtered state after line m_k_final - 1
    KalState m_kal_head;                // newest filtered state (look-ahead head)
    int64_t m_kal_head_k;               // its line, 0 while unset
    int64_t m_k_final;                  // first line not yet finalized
    // Innovation-based adaptation of the period process noise: the filter
    // does not estimate its own noise model, so the normalized innovation
    // variance steers a slow scale factor -- wow varies with disc radius
    double m_qscale;
    double m_nis_avg;
    int64_t m_adapted_to_k;
    // Per-frame sync quality (logged at debug level): how many lines had a
    // sync, and how many of those disagreed with the fitted curve
    int m_frame_resid_bad;
    int m_frame_meas;
    int64_t m_frame_log_k;

    std::deque<double> m_curve;         // finalized T(k), k from m_curve_base
    int64_t m_curve_base;

    // --- vertical anchor: the standard's line numbers on the lattice ---
    // A group of broad pulses starting on a lattice line boundary is field 1
    // (the group spans lines 4-6 on NTSC, 1-3 on PAL); starting half a line
    // in, field 2.
    bool m_anchored;
    bool m_timebase_restarted;          // report the next frame as the first after a signal loss
    int64_t m_line1_k;                  // lattice line of the current frame's line 1
    int64_t m_pending_drift;            // re-anchor hysteresis: last unconfirmed drift
    int64_t m_frame_start_offset;
    double m_frame_period;              // demod samples per line, at frame start

    static constexpr int c_timing_report_frames = 128;
    double m_process_elapsed_ms;
    double m_read_input_elapsed_ms;
    int m_timed_frames;
};

#endif //MUSECPP_SDTVFRAMEREADER_H
