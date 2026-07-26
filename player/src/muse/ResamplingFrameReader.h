// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_RESAMPLINGFRAMEREADER_H
#define MUSECPP_RESAMPLINGFRAMEREADER_H

#include <cstdint>
#include <memory>
#include "FrameReader.h"
#include "MuseRfDemodulator.h"
#include "input/InputReader.h"
#include "util/PercentileFilter.h"
#include "util/ConstExprHelpers.h"
#include "MuseInputBlock.h"

class ResamplingFrameReader : public FrameReader<MuseInputBlock> {
public:
    explicit ResamplingFrameReader(Logger &log, const std::string &executable_dir, musevk::VulkanManager &vulkan_manager,
                                   const std::string &filename, InputFormat input_format,
                                   double sample_rate, double initial_seek_seconds,
                                   bool demodulate, bool benchmark_shaders, bool efm_enabled,
                                   const std::optional<std::string> &output_filename);
    ResamplingFrameReader(const ResamplingFrameReader&) = delete;
    void operator=(const ResamplingFrameReader&) = delete;

    bool initialize(std::vector<std::unique_ptr<MuseInputBlock>> &buffers) override;
    void cleanup() override;
    void seek(double seconds) override;
    void setEfmEnabled(bool enabled) override;

protected:
    void threadFunc() override;

private:
    // takes output_block as parameter in order to fill in efm data when getting a new demodulated block
    bool resample(float *sample_out, uint8_t *dropout_out,
                  double input_samples_per_sample,
                  std::unique_ptr<MuseInputBlock> const &output_block);

    // takes output_block as parameter in order to fill in efm data when getting a new demodulated block
    bool readInput(std::unique_ptr<MuseInputBlock> const &output_block);

    [[nodiscard]] bool process(std::unique_ptr<MuseInputBlock> const &output_block);
    void setUnlocked();

    InputFormat m_input_format;
    bool m_demodulate;
    std::unique_ptr<InputReader> m_input_reader;
    double m_sample_rate;
    int m_input_samples_decimation_rate;

    MuseRfDemodulator *m_demodulator;

    // The input buffer (and also the input dropout buffer) is a multiple of the demodulated block size.
    // (The same is used if reading from file for simplicity.)
    // The total buffer size needs to be a power of two.
    static constexpr int c_number_of_input_sub_buffers = 2;
    static constexpr size_t c_input_sub_buffer_size = MuseDemodulatedBlock::c_video_block_size;
    static constexpr size_t c_input_buffer_size = c_input_sub_buffer_size * c_number_of_input_sub_buffers;
    static_assert((c_input_sub_buffer_size & (c_input_sub_buffer_size - 1)) == 0); // ensure power of two
    static_assert((c_number_of_input_sub_buffers & (c_number_of_input_sub_buffers - 1)) == 0);
    static constexpr size_t c_input_buffer_size_mask = c_input_buffer_size - 1;
    static constexpr unsigned c_input_sub_buffer_size_bits = ConstHelpers::log2(c_input_sub_buffer_size);

    float *m_input_buffer;
    uint8_t *m_input_dropout_buffer;
    long m_input_sub_buffer_input_offsets[c_number_of_input_sub_buffers];

    int m_last_input_sub_buffer_ix_read;
    double m_t;

    // Scale applied once during readInput so that the file-mode and demodulator paths produce
    // comparable values (downstream code expects ~0..255).
    float m_input_scale;
    float m_input_offset;

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

    int m_pixel;
    int m_line;
    PllState m_state;
    float m_line1_frame_pulse_sum;
    float m_line2_frame_pulse_sum;
    PercentileFilter m_upper_percentile_filter;
    PercentileFilter m_lower_percentile_filter;
    float m_max_frame_pulse_sum_difference;
    int m_consecutive_good_syncs;
    int m_missed_line_pulses;
    long m_frame_start_offset;

    double m_error_sum;
};

#endif //MUSECPP_RESAMPLINGFRAMEREADER_H
