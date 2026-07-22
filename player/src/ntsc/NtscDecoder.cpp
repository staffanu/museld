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
  m_noise{-1, -1, -1},
  m_blanking_avg(0),
  m_blanking_sq_avg(0),
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
  m_efm_decoder(log, std::nullopt, std::nullopt),
  m_efm_pcm_processor(log),
  m_frames() {
}

NtscDecoder::~NtscDecoder() {
    while (!m_frames.empty()) {
        delete m_frames.back();
        m_frames.pop_back();
    }
    m_manager.getDevice().destroy(m_first_stage_complete_semaphore);
}

bool NtscDecoder::initialize() {
    // Always keep the two latest frames (required for motion detection) -- pretend we have two already
    // The newest frame is always at index 0
    for (int i = 0; i < 3; i++)
        m_frames.push_back(new NtscFrame(m_log, -i, m_manager));

    for (int i = 0; i < 3; i++)
        for (int parity = 0; parity <= 1; parity++) {
            m_frames[i]->get_field(parity).set_prev_field(
                    &m_frames[parity == 1 ? i : (i + 1) % 2]->get_field(1 - parity));
        }

    m_frame_no = 0;
    m_field_index = 0;
    m_total_elapsed_time_us = 0;

    return true;
}

// For NTSC, enable_non_linear is not implemented
bool NtscDecoder::next(const DecodeControls &controls, DecodedField &out) {
    const bool efm_audio = controls.efm_audio;
    const FieldInterpolationMode field_interpolation_mode = controls.field_interpolation_mode;
    const bool redo_last_field = controls.redo_last_field;
    const DropoutMode dropout_mode = controls.dropout_mode;
    const bool output_yuv = controls.output_yuv;

    out.audio_sample_count = 0;
    out.decoded = false;

    if (redo_last_field) // undo the field advance from the previous call
        m_field_index = (m_field_index + 1) % 2;

    auto t0 = chrono::high_resolution_clock::now();

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
        m_noise_psd_windows += NtscFrame::AccumulateNoisePsd(input_block->video_data->data<float>(),
                                                             m_noise_psd.data(), 3.0f * m_noise.sigma_blanking);
        if (m_frame_no % 30 == 0 && m_noise.sigma_blanking > 0) {
            // 100 IRE = the blanking-to-white span of 0.7 voltage units.  The
            // input is already de-emphasized by the RF demodulator, so this is
            // an unweighted post-de-emphasis figure.
            constexpr float ire = 100.0f / 0.7f;
            double wander_var = m_blanking_sq_avg - m_blanking_avg * m_blanking_avg;
            m_log.info(eDecoder, std::format(
                    "noise: SNR {:.1f} dB over the 100 IRE range "
                    "(σ = {:.2f} IRE at blanking, {:.2f} at sync tip; blanking wander σ = {:.2f} IRE)",
                    20.0f * log10(0.7f / m_noise.sigma_blanking),
                    m_noise.sigma_blanking * ire,
                    m_noise.sigma_sync * ire,
                    sqrt(max(0.0, wander_var)) * ire));
            if (m_noise_psd_windows > 0) {
                // Band-limit to the 4.2 MHz System M video bandwidth and weight
                // with the Rec. 567 unified network (BT.1439 Annex 2 §3:
                // τ = 245 ns, a = 4.5).  The RF demodulator has already applied
                // de-emphasis, so these are the conventional spec-sheet figures;
                // IEC 60857 12.2.2 requires the unweighted blanking-level S/N
                // to be at least 30 dB.
                constexpr double tau = 245e-9, aw = 4.5;
                constexpr double fs = 910.0 * 525.0 * 30.0 / 1.001; // 4 × fsc
                double total = 0, band = 0, weighted = 0;
                for (int k = 0; k < 256; k++) {
                    double f = std::min(k, 256 - k) / 256.0 * fs;
                    double p = m_noise_psd[k] / m_noise_psd_windows;
                    total += p;
                    if (f <= 4.2e6) {
                        band += p;
                        double wt = 2 * M_PI * f * tau;
                        weighted += p * (1 + wt * wt / (aw * aw)) / (1 + (1 + 1 / aw) * (1 + 1 / aw) * wt * wt);
                    }
                }
                total /= 256;
                band /= 256;
                weighted /= 256;
                m_log.info(eDecoder, std::format(
                        "noise spectrum: SNR {:.1f} dB unweighted in 4.2 MHz, {:.1f} dB Rec. 567-weighted "
                        "(weighting gain {:.1f} dB; spectrum total σ = {:.2f} IRE)",
                        20.0 * log10(0.7 / sqrt(band)),
                        20.0 * log10(0.7 / sqrt(weighted)),
                        10.0 * log10(band / weighted),
                        sqrt(total) * ire));
            }
        }

        // The input block data was written by the host, so make sure it is visible on the GPU
        input_block->video_data->synchronizeHostWrites(*m_first_stage_command_buffer);
        input_block->dropout_data->synchronizeHostWrites(*m_first_stage_command_buffer);

        m_shaders.copyToFrame(*m_first_stage_command_buffer, input_block->video_data, input_block->dropout_data,
            frame->data(), dropout_mode);
        frame->data()->synchronizeForHostRead(*m_first_stage_command_buffer); // for disc code processing

        m_shaders.filterColorForFrame(*m_first_stage_command_buffer, frame);
    }
    m_first_stage_command_buffer->submit({}, {}, {m_first_stage_complete_semaphore});

    // if not decoding all fields, we only decode on field 0 (when we read the data), but
    // actually decode the second field.  For field 1, the same field will be shown again.
    // Always begin the batch here since it will wait for the first stage semaphore to complete
    // (or it won't be unsignaled)
    m_second_stage_command_buffer->begin();
    if (m_decode_video && (m_decode_all_fields || m_field_index == 0)) {
        int decoded_field_index = m_decode_all_fields ? m_field_index : 1;

        out.last_frame_buffer_input_offset = m_frames[0]->getInputOffset();
        out.input_samples_per_muse_sample = m_frames[0]->getInputSamplesPerNtscSample();
        out.field_parity = decoded_field_index;

        m_shaders.decodeSingleField(*m_second_stage_command_buffer, m_frames[0]->get_field(decoded_field_index));
        auto fields = vector<reference_wrapper<NtscFieldView>>{
                m_frames[0]->get_field(decoded_field_index),
                m_frames[1 - decoded_field_index]->get_field(1 - decoded_field_index),
                m_frames[1]->get_field(decoded_field_index),
                m_frames[2 - decoded_field_index]->get_field(1 - decoded_field_index)};

        if (m_shaders.decodeTwoFieldsAndDetectMotion(*m_second_stage_command_buffer, fields, true)) {
            m_log.debug(eVideo, std::format("Field {} inter-frame interpolation success", decoded_field_index));
            m_shaders.combineStillAndMovingParts(*m_second_stage_command_buffer,
                                                 field_interpolation_mode == FieldInterpolationMode::eForceIntraField,
                                                 field_interpolation_mode == FieldInterpolationMode::eForceInterFrame,
                                                 decoded_field_index,
                                                 output_yuv);
        } else {
            m_log.warn(eVideo, std::format("Field {} inter-frame interpolation failed -- using intra-field interpolation", decoded_field_index));
            m_shaders.combineStillAndMovingParts(*m_second_stage_command_buffer, /* force field only */ true, /* force inter frame only */ false,
                decoded_field_index, output_yuv);
        }
    }

    m_second_stage_command_buffer->submit({m_first_stage_complete_semaphore}, {vk::PipelineStageFlagBits::eComputeShader}, {});

    m_first_stage_command_buffer->wait();

    if (input_block != nullptr) {
        m_frames[0]->processVbi();
    }

    if (m_decode_audio && m_field_index == 0) {
        if (efm_audio && input_block != nullptr) {
            auto raw = m_efm_decoder.decode(input_block->efm_data, m_frame_no % 30 == 0);
            for (const auto &s : m_efm_pcm_processor.processSamples(raw, m_efm_decoder.preEmphasis())) {
                if (out.audio_sample_count >= MAX_AUDIO_OUTPUT_SAMPLES) break;
                out.audio_samples[out.audio_sample_count].samples[0] = s.samples[0];
                out.audio_samples[out.audio_sample_count].samples[1] = s.samples[1];
                out.audio_samples[out.audio_sample_count].samples[2] = 0;
                out.audio_samples[out.audio_sample_count].samples[3] = 0;
                out.audio_sample_count++;
            }
            out.audio_mode = MODE_EFM;
        } else { // Analog audio
            out.audio_mode = MODE_UNKNOWN;
            out.audio_sample_count = 0;
        }
    }

    if (input_block != nullptr)
        m_reader.returnBuffer(input_block); // EFM audio uses the buffer, so we cannot return it until now

    if (m_second_stage_command_buffer->isSubmitted())
        m_second_stage_command_buffer->wait();

    if (m_timestamp_query_pool != nullptr)
        m_timestamp_statistics.add_timestamps(m_timestamp_query_pool->getTimestamps());

    auto t1 = chrono::high_resolution_clock::now();
    long time_us = chrono::duration_cast<chrono::microseconds>(t1 - t0).count();
    m_total_elapsed_time_us += time_us;
    m_log.info(ePerformance, std::format("Field {} elapsed time {} ms; {} ms/frame",
                                         m_field_index, time_us / 1000,
                                         m_total_elapsed_time_us / 1000 / m_frame_no));

    if (input_status == InputStatus::eBuffersFilled)
        m_field_index = 0; // skip second field -- next field will be from the next frame
    else
        m_field_index = (m_field_index + 1) % 2;

    out.disc_info = m_frames[0]->getVbiData();

    return true;
}

Decoder::SourceDimensions NtscDecoder::getSourceDimensions() const {
    return {NTSC_Y_BUF_WIDTH, NTSC_FIELD_HEIGHT * 2, NTSC_Y_BUF_WIDTH, NTSC_FIELD_HEIGHT};
}

std::optional<Decoder::PixelFileOffsets> NtscDecoder::computePixelFileOffsets(
        int /*field_x*/, int /*field_y*/, int /*field_parity*/,
        long /*buffer_file_offset*/, double /*input_samples_per_muse_sample*/) const {
    return std::nullopt;
}

void NtscDecoder::outputBenchmarkResults() {
    m_timestamp_statistics.print_stats(3);
}

ResultImages NtscDecoder::getResultImages() {
    return m_shaders.getResultImages();
}
