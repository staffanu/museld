//
// Created by staffanu on 5/22/23.
//

#include <string>
#include <map>
#include <chrono>
#include <format>
#include "musevk/VulkanManager.h"
#include "musevk/TimestampQueryPool.h"
#include "SdDecoder.h"
#include "InputReader.h"
#include "SdInputBlock.h"
#include "SdShaders.h"

using namespace std;



SdDecoder::SdDecoder(
        Logger &log, InputReader<SdInputBlock> &reader, musevk::VulkanManager &manager,
        musevk::CommandPool &command_pool, std::string const &executable_dir,
        bool decode_video, bool decode_all_fields, bool decode_audio,
        musevk::TimestampQueryPool *timestamp_query_pool)
: Decoder(),
  m_log(log),
  m_reader(reader),
  m_manager(manager),
  m_shaders(SdShaders(log, executable_dir, manager, command_pool)),
  m_decode_video(decode_video),
  m_decode_all_fields(decode_all_fields),
  m_decode_audio(decode_audio),
  m_timestamp_query_pool(timestamp_query_pool),
  m_eq{-1, -1},
  m_first_stage_complete_semaphore(manager.getDevice().createSemaphore(vk::SemaphoreCreateInfo())),
  m_reset_timestamp_query_pool_command_buffer(command_pool.createCommandBuffer(timestamp_query_pool)),
  m_first_stage_command_buffer(command_pool.createCommandBuffer(timestamp_query_pool)),
  m_second_stage_command_buffer(command_pool.createCommandBuffer(timestamp_query_pool)),
  m_timestamp_statistics(),
  m_frame_no(-1),
  m_field_index(0),
  m_total_elapsed_time_us(0),
  m_efm_decoder(log),
  m_frames() {
}

SdDecoder::~SdDecoder() {
    while (!m_frames.empty()) {
        delete m_frames.back();
        m_frames.pop_back();
    }
    m_manager.getDevice().destroy(m_first_stage_complete_semaphore);
}

bool SdDecoder::initialize() {
    // Always keep the two latest frames (required for motion detection) -- pretend we have two already
    // The newest frame is always at index 0
    for (int i = 0; i < 2; i++)
        m_frames.push_back(new SdFrame(m_log, -i, m_manager));

//    for (int i = 0; i < 3; i++)
//        for (int parity = 0; parity <= 1; parity++) {
//            m_frame_buffers[i]->get_field(parity).set_prev_field(
//                    &m_frame_buffers[parity == 1 ? i : (i + 1) % 3]->get_field(1 - parity));
//        }

    m_frame_no = 0;
    m_field_index = 0;
    m_total_elapsed_time_us = 0;

    return true;
}

bool SdDecoder::next(bool efm_audio, AudioMode *audio_mode,
                       int *sample_count,
                       AudioFrame output_samples[MAX_AUDIO_OUTPUT_SAMPLES],
                       int *field_parity, long *last_frame_buffer_input_offset, double *input_samples_per_muse_sample,
                       shared_ptr<DiscInfo> *disc_info,
                       FieldInterpolationMode field_interpolation_mode,
                       bool redo_last_field, bool enable_non_linear, DropoutMode dropout_mode, bool output_yuv) {
    *sample_count = 0;

    if (redo_last_field) // undo the field advance from the previous call
        m_field_index = (m_field_index + 1) % 2;

    auto t0 = chrono::high_resolution_clock::now();
    if (m_timestamp_query_pool != nullptr)
        m_timestamp_query_pool->resetAndSubmit(*m_reset_timestamp_query_pool_command_buffer);

    std::unique_ptr<SdInputBlock> input_block = nullptr;
    InputStatus input_status = InputStatus::eNormal;
    if (m_field_index == 0 && !redo_last_field) {
        auto frame_buffer = m_frames.back();
        m_frames.pop_back();
        m_frames.push_front(frame_buffer);

        tie(input_block, input_status) = m_reader.getNextInputBuffer();
        switch (input_status) {
            case InputStatus::eEof:
                return false;
            case InputStatus::eTimeout:
                m_log.info(eDecoder, "Input timeout");
                return true;
            default:
                assert(input_block != nullptr);
                break;
        }
        // frame_buffer->set_frame_no(++m_frame_no, input_block->input_offset, input_block->input_samples_per_muse_sample);
        shared_ptr<musevk::VulkanBuffer> input_vulkan_buffer = input_block->video_data;

        // auto eq_estimate = FrameBuffer::EstimateEq(input_vulkan_buffer->data<float>());
//        if (m_eq.first == -1 && m_eq.second == -1)
//            m_eq = eq_estimate;
//        else
//            m_eq = {m_eq.first * 0.9 + eq_estimate.first * 0.1, m_eq.second * 0.9 + eq_estimate.second * 0.1};
        if (m_frame_no % 30 == 0)
            m_log.info(eDecoder, std::format("eq: {}, {}", m_eq.first, m_eq.second));

        m_first_stage_command_buffer->begin();

        // The input block data was written by the host, so make sure it is visible on the GPU
        input_block->video_data->synchronizeHostWrites(*m_first_stage_command_buffer);
        input_block->dropout_data->synchronizeHostWrites(*m_first_stage_command_buffer);

//        m_shaders.applyEqAndDeemphasisAndGamma(*m_first_stage_command_buffer,
//                                               input_vulkan_buffer, input_block->dropout_data,
//                                               frame_buffer->data(),
//                                               m_eq, enable_non_linear, dropout_mode);
//        frame_buffer->data()->synchronizeForHostRead(*m_first_stage_command_buffer); // for disc code processing
//        m_shaders.convertAudioSampleRate(*m_first_stage_command_buffer, frame_buffer->data());
        m_first_stage_command_buffer->submit({}, {}, {m_first_stage_complete_semaphore});
    }

    // if not decoding all fields, we only decode on field 0 (when we read the data), but
    // actually decode the second field.  For field 1, the same field will be shown again.
    // Always begin the batch here since it will wait for the first stage semaphore to complete
    // (or it won't be unsignaled)
    m_second_stage_command_buffer->begin();
    if (m_decode_video && (m_decode_all_fields || m_field_index == 0)) {
        int decoded_field_index = m_decode_all_fields ? m_field_index : 1;

//        *last_frame_buffer_input_offset = m_frames[0]->getInputOffset();
//        *input_samples_per_muse_sample = m_frame_buffers[0]->getInputSamplesPerSdSample();
        *field_parity = decoded_field_index;

//        m_shaders.decodeIntraField(*m_second_stage_command_buffer, m_frame_buffers[0]->get_field(decoded_field_index));
//        auto fields = vector<reference_wrapper<FieldBufferView>>{
//                m_frame_buffers[0]->get_field(decoded_field_index),
//                m_frame_buffers[1 - decoded_field_index]->get_field(1 - decoded_field_index),
//                m_frame_buffers[1]->get_field(decoded_field_index),
//                m_frame_buffers[2 - decoded_field_index]->get_field(1 - decoded_field_index),
//                m_frame_buffers[2]->get_field(decoded_field_index)};
//
//        if (m_shaders.decodeInterFrameAndDetectMotion(*m_second_stage_command_buffer, fields, true)) {
//            m_log.debug(eVideo, std::format("Field {} inter-frame interpolation success", decoded_field_index));
//            m_shaders.combineStillAndMovingParts(*m_second_stage_command_buffer,
//                                                 field_interpolation_mode == FieldInterpolationMode::eForceIntraField,
//                                                 field_interpolation_mode == FieldInterpolationMode::eForceInterFrame,
//                                                 output_yuv);
//        } else {
//            m_log.warn(eVideo, std::format("Field {} inter-frame interpolation failed -- using intra-field interpolation", decoded_field_index));
//            m_shaders.combineStillAndMovingParts(*m_second_stage_command_buffer, /* force field only */ true, /* force inter frame only */ false, output_yuv);
//        }
    }
    if (m_first_stage_command_buffer->isSubmitted())
        m_second_stage_command_buffer->submit({m_first_stage_complete_semaphore}, {vk::PipelineStageFlagBits::eComputeShader}, {});
    else
        m_second_stage_command_buffer->submit({}, {}, {});

    assert(input_block != nullptr == m_first_stage_command_buffer->isSubmitted());
    if (m_first_stage_command_buffer->isSubmitted()) {
        m_first_stage_command_buffer->wait();

        //m_frame_buffers[0]->processDiscCode();
    }

    if (m_decode_audio && m_field_index == 0) {
        if (efm_audio && input_block != nullptr) {
            m_efm_decoder.decode(m_frame_no, input_block->efm_data, input_block->efm_data_size, sample_count, output_samples);
            *audio_mode = MODE_EFM;
        } else { // Analog audio
            *audio_mode = MODE_UNKNOWN;
            *sample_count = 0;
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

    //*disc_code = m_frame_buffers[0]->getDiscCode();

    return true;
}

void SdDecoder::outputBenchmarkResults() {
    m_timestamp_statistics.print_stats(3);
}

ResultImages SdDecoder::getResultImages() {
    return m_shaders.getResultImages();
}
