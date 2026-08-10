// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <string>
#include <map>
#include <chrono>
#include <cmath>
#include <format>
#include "musevk/VulkanManager.h"
#include "musevk/TimestampQueryPool.h"
#include "NtscDecoder.h"
#include "NtscConstants.h"
#include "FrameReader.h"
#include "NtscInputBlock.h"
#include "NtscShaders.h"
#include "util/RobustNoise.h"

using namespace std;

NtscDecoder::NtscDecoder(
        Logger &log, FrameReader<NtscInputBlock> &reader, musevk::VulkanManager &manager,
        musevk::CommandPool &command_pool, std::string const &executable_dir,
        bool decode_video, bool decode_all_fields, bool decode_audio,
        float tint_degrees, float saturation,
        musevk::TimestampQueryPool *timestamp_query_pool)
: Decoder(),
  m_log(log),
  m_reader(reader),
  m_manager(manager),
  m_shaders(NtscShaders(log, executable_dir, manager, command_pool)),
  m_decode_video(decode_video),
  m_decode_all_fields(decode_all_fields),
  m_decode_audio(decode_audio),
  m_timestamp_query_pool(timestamp_query_pool),
  m_eq{1, 0},
  m_noise{-1, -1, -1, -1},
  m_blanking_avg(0),
  m_blanking_sq_avg(0),
  m_white_avg(-1),
  m_white_flag_frames(0),
  m_level_offset_v(0.3f),
  m_level_scale(1.0f / 0.7f),
  m_prev_burst_phase(std::numeric_limits<double>::quiet_NaN()),
  m_burst_coherence_avg(-1),
  m_noise_psd{},
  m_noise_psd_windows(0),
  m_first_stage_complete_semaphore(manager.getDevice().createSemaphore(vk::SemaphoreCreateInfo())),
  m_reset_timestamp_query_pool_command_buffer(command_pool.createCommandBuffer(timestamp_query_pool)),
  m_first_stage_command_buffer(command_pool.createCommandBuffer(timestamp_query_pool)),
  m_second_stage_command_buffer(command_pool.createCommandBuffer(timestamp_query_pool)),
  m_timestamp_statistics(),
  m_frame_no(-1),
  m_field_index(0),
  m_total_elapsed_time_us(0),
  m_cadence(log),
  m_field_buffer_frame_no{-100, -100},
  m_efm_decoder(log, std::nullopt, std::nullopt),
  m_efm_pcm_processor(log),
  m_pending_audio(),
  m_pending_audio_mode(MODE_UNKNOWN),
  m_frames() {
    // 185.8 degrees is the structural 180 (see ntsc_decode_single_field.comp)
    // plus the offset calibrated against the Video Essentials colorbars
    // (sRGB-linearized bar measurements null the mean hue error); the residual
    // is source-dependent (differential phase of the player and disc), which
    // is what --tint adjusts.
    float a = (185.8f + tint_degrees) * (float)M_PI / 180.0f;
    m_rot_re = saturation * sinf(a);
    m_rot_im = saturation * cosf(a);
}

NtscDecoder::~NtscDecoder() {
    while (!m_frames.empty()) {
        delete m_frames.back();
        m_frames.pop_back();
    }
    m_manager.getDevice().destroy(m_first_stage_complete_semaphore);
}

bool NtscDecoder::initialize() {
    // Newest read frame (the lookahead) at index 0, the displayed frame at
    // index 1, and its two-frame history behind it -- pretend they all exist
    // already so the first reads decode blank frames instead of special cases
    for (int i = 0; i < 4; i++)
        m_frames.push_back(new NtscFrame(m_log, -i, m_manager));

    m_frame_no = 0;
    m_field_index = 0;
    m_total_elapsed_time_us = 0;

    return true;
}

// For NTSC, enable_non_linear is not implemented
bool NtscDecoder::next(const DecodeControls &controls, DecodedField &out) {
    const bool efm_audio = controls.efm_audio;
    const bool use_3d_comb = controls.use_3d_comb;
    const FieldInterpolationMode field_interpolation_mode = controls.field_interpolation_mode;
    const bool redo_last_field = controls.redo_last_field;
    const DropoutMode dropout_mode = controls.dropout_mode;
    const bool output_yuv = controls.output_yuv;

    out.audio_sample_count = 0;
    out.decoded = false;

    if (redo_last_field) // undo the field advance from the previous call
        m_field_index = (m_field_index + 1) % 2;

    auto t0 = chrono::high_resolution_clock::now();

    // Closes the current timing section: adds the time since the previous mark
    // to the given bucket.  Skipped code between two marks just leaves the
    // earlier bucket at zero.
    auto t_prev = t0;
    auto section_ms = [&t_prev](double &acc) {
        auto now = chrono::high_resolution_clock::now();
        acc += chrono::duration<double, std::milli>(now - t_prev).count();
        t_prev = now;
    };

    std::unique_ptr<NtscInputBlock> input_block = nullptr;
    InputStatus input_status = InputStatus::eNormal;
    if (m_field_index == 0 && !redo_last_field) {
        tie(input_block, input_status) = m_reader.getNextInputBuffer();
        switch (input_status) {
            case InputStatus::eEof:
                return false;
            case InputStatus::eTimeout:
                m_log.info(eDecoder, "Input timeout");
                return true; // out.decoded stays false: nothing was decoded
            default:
                assert(input_block != nullptr);
                break;
        }
    }
    section_ms(m_sec_input_ms);
    out.decoded = true;

    m_first_stage_command_buffer->begin();

    if (m_timestamp_query_pool != nullptr)
        m_timestamp_query_pool->reset(*m_first_stage_command_buffer);

    if (input_block != nullptr) {
        auto frame = m_frames.back();
        m_frames.pop_back();
        m_frames.push_front(frame);

        frame->set_frame_no(++m_frame_no, input_block->input_offset, input_block->input_samples_per_video_sample);

        auto noise_estimate = NtscFrame::EstimateNoise(input_block->video_data->data<float>());
        if (m_noise.sigma_blanking < 0) {
            m_noise = noise_estimate;
            m_blanking_avg = noise_estimate.blanking_level;
            m_blanking_sq_avg = (double)noise_estimate.blanking_level * noise_estimate.blanking_level;
        } else {
            m_noise.sigma_blanking = m_noise.sigma_blanking * 0.9f + noise_estimate.sigma_blanking * 0.1f;
            m_noise.sigma_sync = m_noise.sigma_sync * 0.9f + noise_estimate.sigma_sync * 0.1f;
            m_blanking_avg = m_blanking_avg * 0.9 + noise_estimate.blanking_level * 0.1;
            m_blanking_sq_avg = m_blanking_sq_avg * 0.9 + (double)noise_estimate.blanking_level * noise_estimate.blanking_level * 0.1;
        }
        // Frame-to-frame burst phase coherence: the subcarrier inverts once
        // per frame, so the mean burst phase should advance by exactly pi.
        // The deviation is the sampling phase error the 3D comb sees.
        if (!std::isnan(m_prev_burst_phase)) {
            double err = std::abs(std::remainder(noise_estimate.burst_phase - m_prev_burst_phase - M_PI, 2 * M_PI));
            m_burst_coherence_avg = m_burst_coherence_avg < 0 ? err : m_burst_coherence_avg * 0.9 + err * 0.1;
        }
        m_prev_burst_phase = noise_estimate.burst_phase;

        if (noise_estimate.white_flag_level >= 0) {
            m_white_avg = m_white_avg < 0 ? noise_estimate.white_flag_level
                                          : m_white_avg * 0.9 + noise_estimate.white_flag_level * 0.1;
            m_white_flag_frames++;
        }

        // Level calibration for the copy shader's rescale to blanking = 0,
        // white = 1: the offset (0 IRE) tracks the measured back porch level,
        // and the gain (100 IRE) comes from the white flag when the disc
        // provides one -- after a settling count, since a single flagged frame
        // is no reference.  Without a flag the nominal 0.7 V span stays.
        if (m_noise.sigma_blanking >= 0)
            m_level_offset_v = (float)m_blanking_avg;
        if (m_white_flag_frames >= 30)
            m_level_scale = std::clamp(1.0f / ((float)m_white_avg - m_level_offset_v), 1.1f, 1.9f);
        m_noise_psd_windows += NtscFrame::AccumulateNoisePsd(input_block->video_data->data<float>(),
                                                             m_noise_psd.data(), 3.0f * m_noise.sigma_blanking);
        if (m_frame_no % 30 == 0 && m_noise.sigma_blanking > 0) {
            // 100 IRE = the blanking-to-white span of 0.7 voltage units.  These
            // sigmas are measured on the raw demodulated baseband, before the
            // frame-domain de-emphasis.
            constexpr float ire = 100.0f / 0.7f;
            double wander_var = m_blanking_sq_avg - m_blanking_avg * m_blanking_avg;
            m_log.info(eDecoder, std::format(
                    "noise: SNR {:.1f} dB over the 100 IRE range "
                    "(σ = {:.2f} IRE at blanking, {:.2f} at sync tip; blanking wander σ = {:.2f} IRE)",
                    20.0f * log10(0.7f / m_noise.sigma_blanking),
                    m_noise.sigma_blanking * ire,
                    m_noise.sigma_sync * ire,
                    sqrt(max(0.0, wander_var)) * ire));
            m_log.info(eDecoder, std::format(
                    "burst: line phase sigma {:.1f} deg, frame-to-frame coherence error {:.1f} deg (EWMA)",
                    noise_estimate.burst_phase_sigma * 180.0 / M_PI,
                    m_burst_coherence_avg * 180.0 / M_PI));
            m_log.info(eDecoder, std::format(
                    "levels: blanking {:.3f} V, white flag {}, rescale gain {:.3f} (nominal {:.3f})",
                    m_blanking_avg,
                    m_white_avg < 0 ? "not seen"
                                    : std::format("{:.3f} V ({} frames)", m_white_avg, m_white_flag_frames),
                    m_level_scale, 1.0f / 0.7f));
            if (m_noise_psd_windows > 0) {
                // Band-limit to the 4.2 MHz System M video bandwidth, apply the
                // frame-domain de-emphasis response (|D|² of the bilinear
                // transform in ntsc_copy_to_frame.comp), and weight with the
                // Rec. 567 unified network (BT.1439 Annex 2 §3: τ = 245 ns,
                // a = 4.5).  IEC 60857 12.2.2 requires ≥ 30 dB unweighted at
                // the video output, i.e. the after-de-emphasis figure.
                constexpr double b0 = 0.436493739, b1 = -0.239713775, a1 = -0.803220036;
                constexpr double tau = 245e-9, aw = 4.5;
                constexpr double fs = 910.0 * 525.0 * 30.0 / 1.001; // 4 × fsc
                double total = 0, band = 0, deemphasized = 0, weighted = 0;
                for (int k = 0; k < 256; k++) {
                    double f = std::min(k, 256 - k) / 256.0 * fs;
                    double p = m_noise_psd[k] / m_noise_psd_windows;
                    total += p;
                    if (f <= 4.2e6) {
                        band += p;
                        double cosw = cos(2 * M_PI * f / fs);
                        double d2 = (b0 * b0 + b1 * b1 + 2 * b0 * b1 * cosw) / (1 + a1 * a1 + 2 * a1 * cosw);
                        deemphasized += p * d2;
                        double wt = 2 * M_PI * f * tau;
                        weighted += p * d2 * (1 + wt * wt / (aw * aw)) / (1 + (1 + 1 / aw) * (1 + 1 / aw) * wt * wt);
                    }
                }
                total /= 256;
                band /= 256;
                deemphasized /= 256;
                weighted /= 256;
                m_log.info(eDecoder, std::format(
                        "noise spectrum: SNR {:.1f} dB unweighted in 4.2 MHz, {:.1f} dB after de-emphasis, "
                        "{:.1f} dB Rec. 567-weighted (spectrum total σ = {:.2f} IRE)",
                        20.0 * log10(0.7 / sqrt(band)),
                        20.0 * log10(0.7 / sqrt(deemphasized)),
                        20.0 * log10(0.7 / sqrt(weighted)),
                        sqrt(total) * ire));
            }
        }

        section_ms(m_sec_noise_ms); // the noise/level estimation reads the mapped input block on the CPU

        // The input block data was written by the host, so make sure it is visible on the GPU
        input_block->video_data->synchronizeHostWrites(*m_first_stage_command_buffer);
        input_block->dropout_data->synchronizeHostWrites(*m_first_stage_command_buffer);

        m_shaders.extendDropouts(*m_first_stage_command_buffer, input_block->dropout_data, frame->dropout_data());
        m_shaders.copyToFrame(*m_first_stage_command_buffer, input_block->video_data, frame->dropout_data(),
            frame->data(), dropout_mode, m_level_offset_v, m_level_scale);
        frame->data()->synchronizeForHostRead(*m_first_stage_command_buffer); // for disc code processing

        m_shaders.detectColorBurstPhase(*m_first_stage_command_buffer, frame);

        // Directional motion masks for the frame about to be displayed
        // (m_frames[1]), with the just-read frame as lookahead.  Thresholds
        // scale with the measured noise; 0.55 approximates how much the
        // frame-domain de-emphasis attenuates the raw blanking sigma.
        float sigma_c = m_noise.sigma_blanking >= 0 ? m_noise.sigma_blanking * m_level_scale * 0.55f : 0.01f;
        m_shaders.detectMotion(*m_first_stage_command_buffer, frame->data(), m_frames[1]->data(),
                               m_frames[2]->data(), m_frames[3]->data(),
                               m_frame_no > 1, max(0.012f, 4.0f * sigma_c), max(0.04f, 10.0f * sigma_c));
    }
    m_first_stage_command_buffer->submit({}, {}, {m_first_stage_complete_semaphore});

    // if not decoding all fields, we only decode on field 0 (when we read the data), but
    // actually decode the second field.  For field 1, the same field will be shown again.
    // Always begin the batch here since it will wait for the first stage semaphore to complete
    // (or it won't be unsignaled)
    m_second_stage_command_buffer->begin();
    if (m_decode_video && (m_decode_all_fields || m_field_index == 0)) {
        int decoded_field_index = m_decode_all_fields ? m_field_index : 1;

        out.last_frame_buffer_input_offset = m_frames[1]->getInputOffset();
        out.input_samples_per_muse_sample = m_frames[1]->getInputSamplesPerNtscSample();
        out.field_parity = decoded_field_index;

        // The illegal-level bounds in the decode shader are scaled from the
        // measured noise; the copy shader's calibrated rescale puts blanking
        // at exactly 0, so they need no offset.  The 0.52 approximates how
        // much the comb and the de-emphasis attenuate the measured raw
        // blanking noise.
        float sigma_out = m_noise.sigma_blanking >= 0 ? m_noise.sigma_blanking * m_level_scale * 0.52f : 0.02f;
        float level_floor = -max(0.02f, 2.5f * sigma_out);
        float level_ceiling = 1.4f;
        // With a film cadence locked, the field's pairing is known exactly:
        // weave unconditionally when the previously decoded field belongs to
        // the same film frame, and when this field starts a new film frame
        // (whose other half is not decoded yet), show the previous film
        // frame once more -- that is what its 3:2 timing asks for anyway,
        // at the cost of one field of latency.
        auto action = NtscCadenceTracker::FieldAction::eAdaptive;
        if (controls.film_mode && m_decode_all_fields
            && field_interpolation_mode == FieldInterpolationMode::eNormal) {
            int displayed = m_frame_no - 1;
            action = m_cadence.actionForField(displayed, decoded_field_index);
            // A weave reads the partner field from the other parity's buffer
            // set; a starvation-skipped field decode leaves that buffer older
            // than the cadence assumes.  (Holds read only the held output
            // image, which is always just "the previous output".)
            if (action == NtscCadenceTracker::FieldAction::eWeave) {
                bool partner_ok = decoded_field_index == 0
                        ? m_field_buffer_frame_no[1] == displayed - 1
                        : m_field_buffer_frame_no[0] == displayed;
                if (!partner_ok)
                    action = NtscCadenceTracker::FieldAction::eAdaptive;
            }
        }

        // Status line for the disc info overlay (V key).  The letter is the
        // film frame within the (A1A2)(A3B1)(B2C1)(C2C3)(D1D2) cycle the
        // output shows -- its runs of 3, 2, 3, 2 fields make the pulldown
        // rhythm visible; a hold re-shows the previous film frame.
        if (!controls.film_mode || !m_decode_all_fields
            || field_interpolation_mode != FieldInterpolationMode::eNormal) {
            out.film_status = "Film: off";
            out.film_status_detail.clear();
        } else if (int phase = m_cadence.phaseForFrame(m_frame_no - 1); phase < 0) {
            out.film_status = "Film: auto";
            out.film_status_detail.clear();
        } else {
            out.film_status = "Film: 3:2";
            if (action == NtscCadenceTracker::FieldAction::eAdaptive) {
                out.film_status_detail = "adapt"; // locked, but this frame's repeat is missing
            } else {
                static constexpr char c_film_frame[5][2] = {
                        {'D', 'A'}, {'A', 'A'}, {'B', 'B'}, {'C', 'C'}, {'C', 'D'}};
                out.film_status_detail = std::format("{} {}",
                        c_film_frame[phase][decoded_field_index],
                        action == NtscCadenceTracker::FieldAction::eWeave ? "weave" : "hold");
            }
        }

        // Selector noise floor: |cs - ct| accumulated over the 19-sample
        // window is ~sigma per sample for plain noise; 15 sigma keeps noise
        // from flipping the comb choice on flat areas.
        m_shaders.decodeSingleField(*m_second_stage_command_buffer, m_frames[1]->get_field(decoded_field_index),
                                    m_frames[2]->data(), m_frames[0]->data(),
                                    m_frames[2]->burst_phase_data(), m_frames[0]->burst_phase_data(),
                                    m_frames[2]->dropout_data(), m_frames[0]->dropout_data(),
                                    dropout_mode, use_3d_comb, m_rot_re, m_rot_im, level_floor, level_ceiling,
                                    15.0f * sigma_out);
        m_field_buffer_frame_no[decoded_field_index] = m_frame_no - 1;
        if (action == NtscCadenceTracker::FieldAction::eHold) {
            // Re-show the previous film frame from the held copy of the last
            // combine output.  The YUV buffers are left as they are, so
            // --write repeats the previous frame -- which is exactly the 3:2
            // presentation.  The field itself was still decoded above: the
            // next field's weave needs it.
            m_shaders.restoreHeldOutput(*m_second_stage_command_buffer);
        } else {
            // Weaving the still parts needs the previous field's decode,
            // which only exists when all fields are decoded
            m_shaders.combineStillAndMovingParts(*m_second_stage_command_buffer,
                    field_interpolation_mode == FieldInterpolationMode::eForceIntraField || !m_decode_all_fields,
                    field_interpolation_mode == FieldInterpolationMode::eForceInterFrame
                            || action == NtscCadenceTracker::FieldAction::eWeave,
                    decoded_field_index, output_yuv);
            // Keep a clean copy (no OSD or subtitles drawn yet) for a
            // following hold to re-show
            m_shaders.saveCombinedOutput(*m_second_stage_command_buffer);
        }
    }

    m_second_stage_command_buffer->submit({m_first_stage_complete_semaphore}, {vk::PipelineStageFlagBits::eComputeShader}, {});
    section_ms(m_sec_record_ms);

    m_first_stage_command_buffer->wait();
    section_ms(m_sec_gpu1_wait_ms);

    if (input_block != nullptr) {
        m_frames[0]->processVbi();

        // Drive the analog audio's CX expansion from the VBI status, unless
        // the user forces it.  The demodulator runs a few frames ahead of the
        // VBI decode, but the flag changes on program boundaries, so the
        // latency does not matter.
        switch (controls.analog_cx) {
            case CxMode::eOff:
                m_reader.setAnalogCx(false);
                break;
            case CxMode::eOn:
                m_reader.setAnalogCx(true);
                break;
            case CxMode::eAuto:
                if (auto cx = m_frames[0]->getVbiData()->cxEnabled(); cx.has_value())
                    m_reader.setAnalogCx(cx.value());
                break;
        }
        section_ms(m_sec_vbi_ms); // processVbi reads the mapped frame buffer on the CPU

        // Film cadence evidence: the per-field differences between the frame
        // just read and its predecessor (both synchronized for host reads, as
        // the VBI processing needs).  The decision this feeds is always about
        // the DISPLAYED frame m_frames[1], so running one frame behind the
        // read costs nothing.
        if (m_decode_video && m_decode_all_fields && m_frame_no > 1) {
            auto diffs = NtscCadenceTracker::MeasureFieldDiffs(
                    m_frames[0]->data()->data<int16_t>(), m_frames[1]->data()->data<int16_t>());
            // Predicted per-sample sigma of the frame buffer data: the raw
            // blanking sigma through the calibrated rescale and the ~0.55
            // attenuation of the frame-domain de-emphasis
            float sigma = m_noise.sigma_blanking >= 0 ? m_noise.sigma_blanking * m_level_scale * 0.55f : -1.0f;
            m_cadence.update(m_frame_no, diffs.d0, diffs.d1, sigma);
        }
        section_ms(m_sec_cadence_ms); // MeasureFieldDiffs reads two mapped frame buffers on the CPU
    }

    if (m_decode_audio && m_field_index == 0) {
        // Deliver the audio held from the previous read (it belongs to the
        // frame being displayed), then decode and hold this block's audio
        out.audio_mode = m_pending_audio_mode;
        for (const auto &s : m_pending_audio) {
            if (out.audio_sample_count >= MAX_AUDIO_OUTPUT_SAMPLES) break;
            out.audio_samples[out.audio_sample_count++] = s;
        }
        m_pending_audio.clear();
        if (efm_audio && input_block != nullptr) {
            auto raw = m_efm_decoder.decode(input_block->efm_data, m_frame_no % 30 == 0);
            for (const auto &s : m_efm_pcm_processor.processSamples(raw, m_efm_decoder.preEmphasis())) {
                AudioFrame f{};
                f.samples[0] = s.samples[0];
                f.samples[1] = s.samples[1];
                m_pending_audio.push_back(f);
            }
            m_pending_audio_mode = MODE_EFM;
        } else if (input_block != nullptr && !input_block->analog_data.empty()) {
            for (const auto &s : input_block->analog_data) {
                AudioFrame f{};
                f.samples[0] = s.samples[0];
                f.samples[1] = s.samples[1];
                m_pending_audio.push_back(f);
            }
            m_pending_audio_mode = MODE_ANALOG;
        } else {
            m_pending_audio_mode = MODE_UNKNOWN;
        }
    }
    section_ms(m_sec_audio_ms);

    if (input_block != nullptr)
        m_reader.returnBuffer(input_block); // EFM audio uses the buffer, so we cannot return it until now

    if (m_second_stage_command_buffer->isSubmitted())
        m_second_stage_command_buffer->wait();
    section_ms(m_sec_gpu2_wait_ms);

    if (m_timestamp_query_pool != nullptr)
        m_timestamp_statistics.add_timestamps(m_timestamp_query_pool->getTimestamps());

    auto t1 = chrono::high_resolution_clock::now();
    long time_us = chrono::duration_cast<chrono::microseconds>(t1 - t0).count();
    m_total_elapsed_time_us += time_us;
    m_log.info(ePerformance, std::format("Field {} elapsed time {} ms; {} ms/frame",
                                         m_field_index, time_us / 1000,
                                         m_frame_no != 0 ? m_total_elapsed_time_us / 1000 / m_frame_no : -1));

    if (m_field_index == 0 && ++m_timed_frames == c_timing_report_frames) {
        const double frame_budget_ms = NtscInputBlock::c_samples_per_video_line
                * NtscInputBlock::c_total_video_lines / NtscInputBlock::c_video_sampling_frequency * 1e3;
        const int n = c_timing_report_frames;
        m_log.info(ePerformance, std::format(
                "decoder avg/frame (budget {:.2f} ms): input {:.2f} ms, noise {:.2f} ms, record {:.2f} ms, "
                "gpu1 wait {:.2f} ms, vbi {:.2f} ms, cadence {:.2f} ms, audio {:.2f} ms, gpu2 wait {:.2f} ms, "
                "total {:.2f} ms",
                frame_budget_ms, m_sec_input_ms / n, m_sec_noise_ms / n, m_sec_record_ms / n,
                m_sec_gpu1_wait_ms / n, m_sec_vbi_ms / n, m_sec_cadence_ms / n, m_sec_audio_ms / n,
                m_sec_gpu2_wait_ms / n,
                (m_sec_input_ms + m_sec_noise_ms + m_sec_record_ms + m_sec_gpu1_wait_ms + m_sec_vbi_ms
                 + m_sec_cadence_ms + m_sec_audio_ms + m_sec_gpu2_wait_ms) / n));
        m_sec_input_ms = m_sec_noise_ms = m_sec_record_ms = m_sec_gpu1_wait_ms = 0;
        m_sec_vbi_ms = m_sec_cadence_ms = m_sec_audio_ms = m_sec_gpu2_wait_ms = 0;
        m_timed_frames = 0;
    }

    if (input_status == InputStatus::eBuffersFilled)
        m_field_index = 0; // skip second field -- next field will be from the next frame
    else
        m_field_index = (m_field_index + 1) % 2;

    out.disc_info = m_frames[1]->getVbiData();
    // Let the disc info overlay show what the CX expander actually does when
    // the user forces it away from the VBI flag
    if (auto vbi = m_frames[1]->getVbiData())
        vbi->setCxOverride(controls.analog_cx == CxMode::eAuto
            ? std::nullopt
            : std::make_optional(controls.analog_cx == CxMode::eOn));

    return true;
}

Decoder::SourceDimensions NtscDecoder::getSourceDimensions() const {
    return {NTSC_Y_BUF_WIDTH, NTSC_FIELD_HEIGHT * 2, NTSC_Y_BUF_WIDTH, NTSC_FIELD_HEIGHT};
}

std::optional<Decoder::PixelFileOffsets> NtscDecoder::computePixelFileOffsets(
        int field_x, int field_y, int field_parity,
        int64_t buffer_file_offset, double input_samples_per_muse_sample) const {
    // Composite frame buffer coordinates: field rows start at line 22 (line
    // 285 for the second field), picture columns at NTSC_FIELD_START_X on
    // the 910-sample 4 fsc grid.  The composite carries no separate chroma
    // samples, so cr/cb are left unset and only the Y offset is reported.
    constexpr int c_field_start_x = 129; // NTSC_FIELD_START_X in the shaders
    int64_t line = 22 + field_y + 263 * (int64_t)field_parity;
    PixelFileOffsets r;
    r.field_start = buffer_file_offset
            + (int64_t)((22 + 263 * (int64_t)field_parity) * NTSC_TOTAL_WIDTH * input_samples_per_muse_sample);
    r.y = buffer_file_offset
            + (int64_t)((line * NTSC_TOTAL_WIDTH + c_field_start_x + field_x) * input_samples_per_muse_sample);
    return r;
}

void NtscDecoder::outputBenchmarkResults() {
    m_timestamp_statistics.print_stats(3);
}

ResultImages NtscDecoder::getResultImages() {
    return m_shaders.getResultImages();
}
