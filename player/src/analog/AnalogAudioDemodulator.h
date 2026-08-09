// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_ANALOGAUDIODEMODULATOR_H
#define AC3RF_DECODE_ANALOGAUDIODEMODULATOR_H

#include <array>
#include <complex>
#include <cstdint>
#include <memory>
#include <vector>
#include "logging/Logger.h"
#include "../filter/FirFilterStage.h"
#include "../filter/ComplexFirFilterStage.h"
#include "../efm/FractionalResampler.h"
#include "../efm/TwoChannelSample.h"
#include "CxExpander.h"

/*
 * Demodulator for the two analog FM audio channels of an NTSC laserdisc RF
 * capture (left carrier 2.3011 MHz, right carrier 2.8125 MHz, 100 kHz
 * deviation).  The structure follows Ac3RfDemodulator: shared half-band
 * decimation of the real RF, then per channel an NCO mix to baseband, a
 * complex half-band cascade plus channel-select lowpass, and an atan2 FM
 * discriminator.  The audio tail decimates further, resamples to the output
 * rate, applies 75 us de-emphasis and optional CX expansion, and converts to
 * 16-bit stereo samples.
 *
 * Each channel carries a squelch driven by discriminator overload (phase
 * steps beyond the deviation clamp), so a missing carrier — e.g. the right
 * channel on discs where the AC3-RF signal replaces it — mutes instead of
 * outputting noise.
 */
class AnalogAudioDemodulator {
public:
    AnalogAudioDemodulator(Logger &log, double input_sample_frequency, int input_block_size,
                           double output_sample_frequency, bool use_simd);
    ~AnalogAudioDemodulator();

    AnalogAudioDemodulator(const AnalogAudioDemodulator &) = delete;
    AnalogAudioDemodulator &operator=(const AnalogAudioDemodulator &) = delete;
    AnalogAudioDemodulator(AnalogAudioDemodulator &&) = delete;
    AnalogAudioDemodulator &operator=(AnalogAudioDemodulator &&) = delete;

    // Demodulates one full input block (the block size passed at construction)
    // and appends the resulting stereo samples to output.
    void demodulate(const float *input_buffer, bool cx_enabled, std::vector<TwoChannelSample> &output);

    [[nodiscard]] bool squelched(int channel) const { return m_channels[channel].squelched; }

private:
    struct ChannelState {
        // NCO
        int phase_step;
        int phase_accumulator;

        // Mixed-down IQ: half-band cascade + channel-select lowpass
        std::vector<ComplexFirFilterStage *> post_mix_filter_stages;
        std::vector<float> iq_re_buffer;
        std::vector<float> iq_im_buffer;

        // FM discriminator
        std::complex<float> prev_iq;
        int64_t clamped_samples;

        // Audio decimation down to the pre-resampler rate
        std::vector<FirFilterStage *> audio_filter_stages;
        std::vector<float> audio_buffer;
        std::unique_ptr<FractionalResampler> resampler;

        // Post-processing state at the output rate
        float deemph_x1, deemph_y1;
        float dc_x1, dc_y1;

        bool squelched;
        float squelch_gain;
    };

    void buildChannel(ChannelState &channel, const std::string &name, double carrier_frequency,
                      double mix_frequency, bool use_simd);
    void processChannel(ChannelState &channel, std::vector<float> &resampled);

    Logger &m_log;
    double m_input_sample_frequency;
    int m_input_block_size;
    double m_output_sample_frequency;

    static constexpr double c_left_carrier_frequency = 2.3011e6;
    static constexpr double c_right_carrier_frequency = 2.8125e6;
    static constexpr double c_frequency_deviation = 100e3;
    // Discriminator output in deviation units (1.0 = 100 kHz) is clamped here,
    // matching the +-0.5 radian clamp of the hardware implementation.
    static constexpr float c_deviation_clamp = 1.223f;
    // Output level calibration.  Mapping the deviation clamp to int16 full
    // scale plays ~10 dB louder than the EFM track of the same program:
    // discs modulate only a fraction of the +-100 kHz peak deviation, while
    // the digital track is mastered against digital full scale.  -10 dB
    // aligns the two (measured on a CX disc with expansion enabled); the
    // headroom given up is far above the medium's noise floor.
    static constexpr float c_output_level = 0.316f;

    int m_pre_mix_decimation_factor;
    int m_post_mix_decimation_factor;
    int m_audio_decimation_factor;
    double m_audio_sample_frequency; // rate into the fractional resampler

    std::vector<FirFilterStage *> m_pre_mix_filter_stages;
    std::vector<float> m_pre_mix_buffer;

    constexpr static int c_phase_accum_bits = 14;
    std::array<std::complex<float>, 1 << c_phase_accum_bits> m_exp_lut;

    std::array<ChannelState, 2> m_channels;
    std::vector<float> m_resampled[2];

    // De-emphasis and DC blocker coefficients at the output rate
    float m_deemph_b, m_deemph_a1;
    float m_dc_pole;
    float m_squelch_ramp_step;

    CxExpander m_cx_expander;
};

#endif //AC3RF_DECODE_ANALOGAUDIODEMODULATOR_H
