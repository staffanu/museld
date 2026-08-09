// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_NTSCFRAMEREADER_H
#define MUSECPP_NTSCFRAMEREADER_H

#include <cstdint>
#include "FrameReader.h"
#include "NtscRfDemodulator.h"
#include "input/InputReader.h"
#include "util/PercentileFilter.h"
#include "util/ConstExprHelpers.h"
#include "NtscInputBlock.h"

class NtscFrameReader : public FrameReader<NtscInputBlock> {
public:
    explicit NtscFrameReader(Logger &log, const std::string &executable_dir, musevk::VulkanManager &vulkan_manager,
                             const std::string &filename, InputFormat input_format,
                             double sample_rate, double initial_seek_seconds,
                             bool benchmark_shaders, bool efm_enabled,
                             const std::optional<std::string> &output_filename);
    NtscFrameReader(const NtscFrameReader&) = delete;
    void operator=(const NtscFrameReader&) = delete;
    // Join the reader thread while threadFunc() and the demodulator still
    // exist; the base destructor's cleanup() would be too late.
    ~NtscFrameReader() override {
        NtscFrameReader::cleanup();
    }

    bool initialize(std::vector<std::unique_ptr<NtscInputBlock>> &buffers) override;
    void cleanup() override;
    void seek(double seconds) override;
    void setEfmEnabled(bool enabled) override;
    void setAnalogCx(bool enabled) override;

protected:
    void threadFunc() override;

private:
    enum PulseType { NormalSync, EqualizationPulse, BroadPulse };
    int expectedPulseSamplesForHalfLine(int line, int half);

    // takes output_block as parameter in order to fill in efm data when getting a new demodulated block
    bool resample(float *sample_out, uint8_t *dropout_out,
                  double input_samples_per_sample,
                  std::unique_ptr<NtscInputBlock> const &output_block);

    // takes output_block as parameter in order to fill in efm data when getting a new demodulated block
    bool readInput(std::unique_ptr<NtscInputBlock> const &output_block);

    void updateFrameStartOffset();

    [[nodiscard]] bool process(std::unique_ptr<NtscInputBlock> const &output_block);
    void setUnlocked();

    int m_file_fd;
    double m_sample_rate;
    int m_input_samples_decimation_rate;

    NtscRfDemodulator *m_demodulator;

    // The input buffer (and also the input dropout buffer) is a multiple of the demodulated block size.
    // (The same is used if reading from file for simplicity.)
    // The total buffer size needs to be a power of two.
    static constexpr int c_number_of_input_sub_buffers = 2;
    static constexpr size_t c_input_sub_buffer_size = NtscRfDemodulatorConstants::c_video_block_size;
    static constexpr size_t c_input_buffer_size = c_input_sub_buffer_size * c_number_of_input_sub_buffers;
    static_assert((c_input_sub_buffer_size & (c_input_sub_buffer_size - 1)) == 0); // ensure power of two
    static_assert((c_number_of_input_sub_buffers & (c_number_of_input_sub_buffers - 1)) == 0);
    static constexpr size_t c_input_buffer_size_mask = c_input_buffer_size - 1;
    static constexpr unsigned c_input_sub_buffer_size_bits = ConstHelpers::log2(c_input_sub_buffer_size);

    uint8_t *m_input_buffer;
    uint8_t *m_input_dropout_buffer;
    int64_t m_input_sub_buffer_input_offsets[c_number_of_input_sub_buffers];

    int m_last_input_sub_buffer_ix_read;
    double m_t;

    int m_bytes_per_sample;
    double m_output_multiplier;
    double m_output_add;

    double m_input_samples_per_sample_ref;
    double m_input_samples_per_sample;
    // These variable names and the computations performed are heavily influenced by
    // the TI publication "Introduction to phase-locked loop system modeling"
    // (Analog Applications Journal SLYT015 - May 2000 Analog and Mixed-Signal Products)
    double m_omega; // undamped frequency
    double m_zeta; // damping factor
    double m_Ts; // We think of one line as one sample here since we detect the phase once per line
    double m_G1;
    double m_G2;
    double m_GpdGvco;
    double m_g1;
    double m_g2;

    enum PllState {
        eSearching, eLocked, eLockedHoriz
    };

    static constexpr double c_normalPulseTime = 4.7e-6;
    static constexpr double c_equalizationPulseTime = c_normalPulseTime / 2;
    static constexpr double c_broadPulseTime =  NtscInputBlock::c_samples_per_video_line / NtscInputBlock::c_video_sampling_frequency - c_normalPulseTime;

    const int c_normalPulseSamples = c_normalPulseTime * NtscInputBlock::c_video_sampling_frequency;
    const int c_equalizationPulseSamples = c_equalizationPulseTime * NtscInputBlock::c_video_sampling_frequency;
    const int c_broadPulseSamples = c_broadPulseTime * NtscInputBlock::c_video_sampling_frequency;

    float m_half_line_sync_pattern_error_sum; // used when locked
    float m_half_line_sync_pattern_eq_error_sum; // used when scanning for horiz sync
    float m_half_line_sync_pattern_br_error_sum; // used when scanning for horiz sync
    // two bits per half line: 01: EQ, 11: BR, 00 -- other. Last half line shifted in at LSB
    // (uses 36 bits, so a fixed 64-bit type: long is 32-bit on Windows)
    uint64_t m_vert_sync_half_line_pattern;

    int m_sample_ix;
    int m_line;
    PllState m_state;
    PercentileFilter m_lower_percentile_filter;
    int m_consecutive_good_syncs;
    int m_missed_half_line_vert_sync_patterns;
    // Set when the lock landed on field 2's vertical interval, so the frame
    // being filled has no field 1 and must not be handed on
    bool m_first_field_incomplete;
    int64_t m_frame_start_offset;

    static constexpr int c_sample_history_size = 16;
    float m_sample_history[c_sample_history_size];
    int m_sample_history_ix;

    double m_error_sum;

    // Per-frame CPU timing of the reader thread, reported every
    // c_timing_report_frames frames: the DPLL/sync loop's own time versus the
    // time spent in readInput (fetching demodulated blocks, which blocks on
    // the demodulator when it is the slower stage)
    static constexpr int c_timing_report_frames = 128;
    double m_process_elapsed_ms;
    double m_read_input_elapsed_ms;
    int m_timed_frames;
};

#endif //MUSECPP_NTSCFRAMEREADER_H
