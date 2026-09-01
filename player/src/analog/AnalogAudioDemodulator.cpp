// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <format>
#include <numeric>
#include "../filter/HalfBand.h"
#include "../filter/KaiserLowPass.h"
#include "AnalogAudioDemodulator.h"

AnalogAudioDemodulator::AnalogAudioDemodulator(Logger &log, double input_sample_frequency, int input_block_size,
                                               double output_sample_frequency, bool use_simd)
: m_log(log),
  m_input_sample_frequency(input_sample_frequency),
  m_input_block_size(input_block_size),
  m_output_sample_frequency(output_sample_frequency),
  m_cx_expander(output_sample_frequency)
{
    if (input_sample_frequency < 6.5e6)
        throw std::runtime_error("Analog audio decoding needs an input sample frequency of at least 6.5 MHz");

    // Both carriers with their FM sidebands fit below 3.05 MHz, the same upper
    // band edge as AC3-RF, so the same pre-mix rule applies: decimate as far as
    // possible while keeping at least 7 MHz.
    int pre_mix_log2_decimation = std::max(0, (int)floor(::log(input_sample_frequency / 7e6) / ::log(2)));
    m_pre_mix_decimation_factor = 1 << pre_mix_log2_decimation;
    double mix_frequency = input_sample_frequency / m_pre_mix_decimation_factor;

    // After mixing a carrier to baseband, decimate to at least 1 MHz.  The FM
    // discriminator runs there: with 100 kHz deviation the legitimate phase
    // step stays well below pi, leaving clamp hits as a clean squelch signal.
    int post_mix_log2_decimation = (int)floor(::log(mix_frequency / 1e6) / ::log(2));
    assert(post_mix_log2_decimation >= 1);
    m_post_mix_decimation_factor = 1 << post_mix_log2_decimation;
    double discriminator_frequency = mix_frequency / m_post_mix_decimation_factor;

    // Decimate the demodulated audio to at least 125 kHz before the fractional
    // resampler; the 20 kHz lowpass in front of it removes the rising FM noise.
    int audio_log2_decimation = (int)floor(::log(discriminator_frequency / 125e3) / ::log(2));
    assert(audio_log2_decimation >= 1);
    m_audio_decimation_factor = 1 << audio_log2_decimation;
    m_audio_sample_frequency = discriminator_frequency / m_audio_decimation_factor;

    const int total_decimation = m_pre_mix_decimation_factor * m_post_mix_decimation_factor * m_audio_decimation_factor;
    if (m_input_block_size % total_decimation != 0)
        throw std::runtime_error("Analog audio input block size must be a multiple of the total decimation");

    m_pre_mix_buffer.resize(m_input_block_size / m_pre_mix_decimation_factor);

    // Shared pre-mix decimation of the real RF signal, built in reverse so each
    // stage can point at the next stage's input buffer.
    m_pre_mix_filter_stages.resize(pre_mix_log2_decimation);
    for (int i = pre_mix_log2_decimation - 1; i >= 0; i--) {
        double stage_sample_freq = input_sample_frequency / (1 << i);
        const int ntaps = 19;
        std::vector<float> filter = HalfBand::kaiser<float>(ntaps, HalfBand::kBeta80dB);
        std::string description = std::format(
            "Half-band Kaiser low-pass Fs={}, ntaps={}, beta={}",
            stage_sample_freq, ntaps, HalfBand::kBeta80dB);
        m_pre_mix_filter_stages[i] = new FirFilterStage(
            std::format("Pre mix decimate by 2 stage {}", i + 1),
            description,
            filter,
            2,
            m_input_block_size >> i,
            i == pre_mix_log2_decimation - 1 ? 0 : m_pre_mix_filter_stages[i + 1]->filterSize() - 1,
            i == pre_mix_log2_decimation - 1 ? &m_pre_mix_buffer : m_pre_mix_filter_stages[i + 1]->inputBuffer(),
            use_simd);
    }

    // Create a lookup table for exp(i * 2 * pi * t); the two channels share it
    // with their own phase accumulators.
    for (int i = 0; i < 1 << c_phase_accum_bits; i++)
        m_exp_lut[i] = std::polar(1.0, 2.0 * M_PI * i / (1 << c_phase_accum_bits));

    buildChannel(m_channels[0], "left", c_left_carrier_frequency, mix_frequency, use_simd);
    buildChannel(m_channels[1], "right", c_right_carrier_frequency, mix_frequency, use_simd);

    // 75 us de-emphasis via the bilinear transform, and a 5 Hz DC blocker that
    // removes the offset a carrier frequency error leaves after the discriminator.
    const double k = 2.0 * output_sample_frequency * 75e-6;
    m_deemph_b = (float)(1.0 / (1.0 + k));
    m_deemph_a1 = (float)((1.0 - k) / (1.0 + k));
    m_dc_pole = (float)exp(-2.0 * M_PI * 5.0 / output_sample_frequency);
    m_squelch_ramp_step = (float)(1.0 / (0.005 * output_sample_frequency)); // 5 ms mute/unmute ramp

    m_log.debug(eAudio, std::format(
        "Analog audio: mix Fs={}, discriminator Fs={}, pre-resampler Fs={}, output Fs={}",
        mix_frequency, discriminator_frequency, m_audio_sample_frequency, output_sample_frequency));
    for (const auto &stage: m_pre_mix_filter_stages)
        m_log.debug(eAudio, std::format("Filter stage: {}", stage->toString()));
    for (const auto &stage: m_channels[0].post_mix_filter_stages)
        m_log.debug(eAudio, std::format("Filter stage: {}", stage->toString()));
    for (const auto &stage: m_channels[0].audio_filter_stages)
        m_log.debug(eAudio, std::format("Filter stage: {}", stage->toString()));
}

void AnalogAudioDemodulator::buildChannel(ChannelState &channel, const std::string &name, double carrier_frequency,
                                          double mix_frequency, bool use_simd) {
    channel.name = name;
    channel.phase_step = (int)((1 << c_phase_accum_bits) * carrier_frequency / mix_frequency);
    channel.phase_accumulator = 0;
    channel.prev_iq = {0.0f, 0.0f};
    channel.clamped_samples = 0;
    channel.deemph_x1 = channel.deemph_y1 = 0;
    channel.dc_x1 = channel.dc_y1 = 0;
    channel.squelched = true; // muted until the first block confirms a carrier
    channel.squelch_gain = 0;

    const int post_mix_log2_decimation = (int)std::round(::log((double)m_post_mix_decimation_factor) / ::log(2));
    const int audio_log2_decimation = (int)std::round(::log((double)m_audio_decimation_factor) / ::log(2));
    const int pre_mix_block_size = m_input_block_size / m_pre_mix_decimation_factor;
    const double discriminator_frequency = mix_frequency / m_post_mix_decimation_factor;

    channel.iq_re_buffer.resize(pre_mix_block_size / m_post_mix_decimation_factor);
    channel.iq_im_buffer.resize(pre_mix_block_size / m_post_mix_decimation_factor);
    channel.audio_buffer.resize(pre_mix_block_size / m_post_mix_decimation_factor / m_audio_decimation_factor);

    // Channel-select lowpass at the discriminator rate: pass the FM signal
    // (+-150 kHz), stop by +-480 kHz where the neighboring carrier sits after
    // mixing (the carriers are 511 kHz apart).
    channel.post_mix_filter_stages.resize(post_mix_log2_decimation + 1);
    {
        std::vector<float> filter = KaiserLowPass::design<float>(discriminator_frequency, 315e3, 330e3, 80);
        std::string description = std::format(
            "Kaiser low-pass Fs={}, cutoff=315 kHz, ntaps={}", discriminator_frequency, filter.size());
        channel.post_mix_filter_stages[post_mix_log2_decimation] = new ComplexFirFilterStage(
            std::format("{} channel select filter", name),
            description,
            filter,
            1,
            pre_mix_block_size >> post_mix_log2_decimation,
            0,
            &channel.iq_re_buffer,
            &channel.iq_im_buffer,
            use_simd);
    }
    for (int i = post_mix_log2_decimation - 1; i >= 0; i--) {
        double stage_sample_freq = mix_frequency / (1 << i);
        const int ntaps = 19;
        std::vector<float> filter = HalfBand::kaiser<float>(ntaps, HalfBand::kBeta80dB);
        std::string description = std::format(
            "Half-band Kaiser low-pass Fs={}, ntaps={}, beta={}",
            stage_sample_freq, ntaps, HalfBand::kBeta80dB);
        channel.post_mix_filter_stages[i] = new ComplexFirFilterStage(
            std::format("{} post mix decimate by 2 stage {}", name, i + 1),
            description,
            filter,
            2,
            pre_mix_block_size >> i,
            channel.post_mix_filter_stages[i + 1]->filterSize() - 1,
            channel.post_mix_filter_stages[i + 1]->inputReBuffer(),
            channel.post_mix_filter_stages[i + 1]->inputImBuffer(),
            use_simd);
    }

    // Audio tail: half-band decimation from the discriminator rate, then a
    // final lowpass flat to 20 kHz and 80 dB down by 50 kHz in front of the
    // fractional resampler.
    const int discriminator_block_size = pre_mix_block_size / m_post_mix_decimation_factor;
    channel.audio_filter_stages.resize(audio_log2_decimation + 1);
    {
        std::vector<float> filter = KaiserLowPass::design<float>(m_audio_sample_frequency, 35e3, 30e3, 80);
        std::string description = std::format(
            "Kaiser low-pass Fs={}, cutoff=35 kHz, ntaps={}", m_audio_sample_frequency, filter.size());
        channel.audio_filter_stages[audio_log2_decimation] = new FirFilterStage(
            std::format("{} audio lowpass", name),
            description,
            filter,
            1,
            discriminator_block_size >> audio_log2_decimation,
            0,
            &channel.audio_buffer,
            use_simd);
    }
    for (int i = audio_log2_decimation - 1; i >= 0; i--) {
        double stage_sample_freq = discriminator_frequency / (1 << i);
        const int ntaps = 19;
        std::vector<float> filter = HalfBand::kaiser<float>(ntaps, HalfBand::kBeta80dB);
        std::string description = std::format(
            "Half-band Kaiser low-pass Fs={}, ntaps={}, beta={}",
            stage_sample_freq, ntaps, HalfBand::kBeta80dB);
        channel.audio_filter_stages[i] = new FirFilterStage(
            std::format("{} audio decimate by 2 stage {}", name, i + 1),
            description,
            filter,
            2,
            discriminator_block_size >> i,
            channel.audio_filter_stages[i + 1]->filterSize() - 1,
            channel.audio_filter_stages[i + 1]->inputBuffer(),
            use_simd);
    }

    channel.resampler = std::make_unique<FractionalResampler>((int)channel.audio_buffer.size());
}

AnalogAudioDemodulator::~AnalogAudioDemodulator() {
    for (const auto stage: m_pre_mix_filter_stages)
        delete stage;
    m_pre_mix_filter_stages.clear();
    for (auto &channel: m_channels) {
        for (const auto stage: channel.post_mix_filter_stages)
            delete stage;
        channel.post_mix_filter_stages.clear();
        for (const auto stage: channel.audio_filter_stages)
            delete stage;
        channel.audio_filter_stages.clear();
    }
}

void AnalogAudioDemodulator::processChannel(ChannelState &channel, std::vector<float> &resampled) {
    const size_t n_pre_mix = m_pre_mix_buffer.size();

    // Mix with exp(-i * 2 * pi * f_carrier * t) to move the carrier to baseband
    float *first_stage_input_re = channel.post_mix_filter_stages[0]->inputReBuffer()->data() +
        channel.post_mix_filter_stages[0]->filterSize() - 1;
    float *first_stage_input_im = channel.post_mix_filter_stages[0]->inputImBuffer()->data() +
        channel.post_mix_filter_stages[0]->filterSize() - 1;
    for (size_t i = 0; i < n_pre_mix; i++) {
        const std::complex<float> &lo = m_exp_lut[channel.phase_accumulator];
        first_stage_input_re[i] = m_pre_mix_buffer[i] * lo.real();
        first_stage_input_im[i] = -m_pre_mix_buffer[i] * lo.imag();
        channel.phase_accumulator = (channel.phase_accumulator + channel.phase_step) & ((1 << c_phase_accum_bits) - 1);
    }

    // Decimate the IQ signal and apply the channel-select lowpass
    {
        size_t n = n_pre_mix;
        for (const auto stage: channel.post_mix_filter_stages) {
            stage->applyFilter(static_cast<int>(n));
            n /= stage->decimationFactor();
        }
    }

    // FM discriminator: the angle between consecutive IQ samples, scaled to
    // deviation units (1.0 = 100 kHz) and clamped.  Clamp hits feed the
    // squelch; the clamp sits where only a missing carrier's noise reaches
    // (see c_deviation_clamp), so program peaks pass untouched and never
    // mute the channel.
    const size_t n_iq = channel.iq_re_buffer.size();
    const float discriminator_frequency =
        (float)(m_input_sample_frequency / m_pre_mix_decimation_factor / m_post_mix_decimation_factor);
    const float scale = discriminator_frequency / (float)(2.0 * M_PI * c_frequency_deviation);
    float *audio_chain_input = channel.audio_filter_stages[0]->inputBuffer()->data() +
        channel.audio_filter_stages[0]->filterSize() - 1;
    channel.clamped_samples = 0;
    for (size_t i = 0; i < n_iq; i++) {
        const std::complex<float> iq(channel.iq_re_buffer[i], channel.iq_im_buffer[i]);
        const std::complex<float> rotation = iq * std::conj(channel.prev_iq);
        channel.prev_iq = iq;
        float d = std::atan2(rotation.imag(), rotation.real()) * scale;
        if (d >= c_deviation_clamp) {
            d = c_deviation_clamp;
            channel.clamped_samples++;
        } else if (d <= -c_deviation_clamp) {
            d = -c_deviation_clamp;
            channel.clamped_samples++;
        }
        audio_chain_input[i] = d;
    }

    // Decimate the audio down to the pre-resampler rate
    {
        size_t n = n_iq;
        for (const auto stage: channel.audio_filter_stages) {
            stage->applyFilter(static_cast<int>(n));
            n /= stage->decimationFactor();
        }
    }

    // Move the overlap tails to the front of the filters' input buffers
    {
        size_t n = n_pre_mix;
        for (auto stage: channel.post_mix_filter_stages) {
            stage->moveDataToFront(static_cast<int>(n));
            n /= stage->decimationFactor();
        }
    }
    {
        size_t n = n_iq;
        for (auto stage: channel.audio_filter_stages) {
            stage->moveDataToFront(static_cast<int>(n));
            n /= stage->decimationFactor();
        }
    }

    // Squelch with hysteresis on the fraction of overflowed discriminator
    // samples (the software analog of the hardware's angle-overflow counter)
    const double clamp_fraction = (double)channel.clamped_samples / (double)n_iq;
    const bool was_squelched = channel.squelched;
    if (clamp_fraction > 1.0 / 16)
        channel.squelched = true;
    else if (clamp_fraction < 1.0 / 128)
        channel.squelched = false;
    if (channel.squelched != was_squelched)
        m_log.info(eAudio, std::format("Analog audio {} channel squelch {} (overflow fraction {:.4f})",
                                       channel.name, channel.squelched ? "on" : "off", clamp_fraction));

    // Resample to the output rate
    resampled.clear();
    channel.resampler->updateInput(channel.audio_buffer.data());
    const double dt = m_audio_sample_frequency / m_output_sample_frequency;
    float sample;
    while (channel.resampler->advanceTimeAndResample(dt, sample))
        resampled.push_back(sample);
}

void AnalogAudioDemodulator::demodulate(const float *input_buffer, bool cx_enabled,
                                        std::vector<TwoChannelSample> &output) {
    // Low-pass filter and decimate the input signal (shared by both channels)
    if (m_pre_mix_filter_stages.empty()) {
        memcpy(m_pre_mix_buffer.data(), input_buffer, m_input_block_size * sizeof(float));
    } else {
        float *first_stage_input_buffer = m_pre_mix_filter_stages[0]->inputBuffer()->data() +
            m_pre_mix_filter_stages[0]->filterSize() - 1;
        memcpy(first_stage_input_buffer, input_buffer, m_input_block_size * sizeof(float));

        size_t n = m_input_block_size;
        for (const auto stage: m_pre_mix_filter_stages) {
            stage->applyFilter(static_cast<int>(n));
            n /= stage->decimationFactor();
        }
        n = m_input_block_size;
        for (auto stage: m_pre_mix_filter_stages) {
            stage->moveDataToFront(static_cast<int>(n));
            n /= stage->decimationFactor();
        }
    }

    processChannel(m_channels[0], m_resampled[0]);
    processChannel(m_channels[1], m_resampled[1]);
    assert(m_resampled[0].size() == m_resampled[1].size());

    // De-emphasis, DC blocking, CX expansion, squelch and conversion to s16
    for (size_t i = 0; i < m_resampled[0].size(); i++) {
        float samples[2];
        for (int c = 0; c < 2; c++) {
            ChannelState &channel = m_channels[c];
            const float x = m_resampled[c][i];

            const float deemph = m_deemph_b * (x + channel.deemph_x1) - m_deemph_a1 * channel.deemph_y1;
            channel.deemph_x1 = x;
            channel.deemph_y1 = deemph;

            const float dc_blocked = deemph - channel.dc_x1 + m_dc_pole * channel.dc_y1;
            channel.dc_x1 = deemph;
            channel.dc_y1 = dc_blocked;

            samples[c] = dc_blocked;
        }

        // The sidechain runs whether or not CX is enabled so the level
        // estimate stays warm across transitions
        const float cx_gain = m_cx_expander.processSidechain(samples[0], samples[1]);
        if (cx_enabled) {
            samples[0] *= cx_gain;
            samples[1] *= cx_gain;
        }

        TwoChannelSample out_sample{};
        for (int c = 0; c < 2; c++) {
            ChannelState &channel = m_channels[c];
            const float squelch_target = channel.squelched ? 0.0f : 1.0f;
            if (channel.squelch_gain < squelch_target)
                channel.squelch_gain = std::min(squelch_target, channel.squelch_gain + m_squelch_ramp_step);
            else if (channel.squelch_gain > squelch_target)
                channel.squelch_gain = std::max(squelch_target, channel.squelch_gain - m_squelch_ramp_step);

            const float scaled = samples[c] * channel.squelch_gain * (32767.0f * c_output_level);
            out_sample.samples[c] = (int16_t)std::clamp(scaled, -32768.0f, 32767.0f);
        }
        output.push_back(out_sample);
    }
}
