//
// Created by staffanu on 5/22/23.
//

#include <string>
#include <map>
#include <chrono>
#include <fmt/format.h>
#include "MuseTypes.h"
#include "MuseDecoder.h"
#include "FrameBuffer.h"
#include "musevk/VulkanManager.h"
#include "musevk/TimestampQueryPool.h"

using namespace std;

MuseDecoder::MuseDecoder(
        Logger &log, InputReader &reader, Shaders &shaders, musevk::VulkanManager &manager,
        bool decode_video, bool decode_all_fields, bool decode_audio,
        musevk::TimestampQueryPool *timestamp_query_pool)
: m_log(log),
  m_reader(reader),
  m_shaders(shaders),
  m_manager(manager),
  m_decode_video(decode_video),
  m_decode_all_fields(decode_all_fields),
  m_decode_audio(decode_audio),
  m_timestamp_query_pool(timestamp_query_pool),
  m_eq{-1, -1},
  m_first_stage_complete_semaphore(manager.getDevice().createSemaphore(vk::SemaphoreCreateInfo())),
  m_reset_timestamp_query_pool_command_buffer(m_manager.createCommandBuffer(timestamp_query_pool)),
  m_first_stage_command_buffer(m_manager.createCommandBuffer(timestamp_query_pool)),
  m_second_stage_command_buffer(m_manager.createCommandBuffer(timestamp_query_pool)),
  m_audio_decoder(log) {
}

MuseDecoder::~MuseDecoder() {
    while (!m_frame_buffers.empty()) {
        delete m_frame_buffers.back();
        m_frame_buffers.pop_back();
    }
    m_manager.getDevice().destroy(m_first_stage_complete_semaphore);
}

bool MuseDecoder::initialize() {
    // Always keep the three latest frames (required for motion detection) -- pretend we have three already
    // The newest frame is always at index 0
    for (int i = 0; i < 3; i++)
        m_frame_buffers.push_back(new FrameBuffer(m_log, -i,
                                                  m_shaders.createMuseBuffer(MUSE_TOTAL_HEIGHT, MUSE_TOTAL_WIDTH)));

    for (int i = 0; i < 3; i++)
        for (int parity = 0; parity <= 1; parity++) {
            m_frame_buffers[i]->get_field(parity).set_prev_field(
                    &m_frame_buffers[parity == 1 ? i : (i + 1) % 3]->get_field(1 - parity));
        }

    m_frame_no = 0;
    m_field_index = 0;
    m_total_elapsed_time_us = 0;

    return true;
}

bool MuseDecoder::next(AudioDecoder::AudioMode &audio_mode,
                       size_t &sample_count,
                       AudioDecoder::AudioFrame output_samples[AudioDecoder::c_max_output_samples],
                       FieldInterpolationMode field_interpolation_mode,
                       bool redo_last_field) {
    if (redo_last_field)
        m_field_index = (m_field_index + 1) % 2;

    auto t0 = chrono::high_resolution_clock::now();
    if (m_timestamp_query_pool != nullptr) {
        m_reset_timestamp_query_pool_command_buffer->begin();
        m_timestamp_query_pool->reset(*m_reset_timestamp_query_pool_command_buffer);
        m_timestamp_query_pool->timestamp(*m_reset_timestamp_query_pool_command_buffer, "begin", vk::PipelineStageFlagBits::eTopOfPipe);
        m_reset_timestamp_query_pool_command_buffer->submit({}, {}, {});
        m_reset_timestamp_query_pool_command_buffer->wait();
    }

    shared_ptr<musevk::VulkanBuffer> input_vulkan_buffer = nullptr;
    InputReader::PresentationHint presentationHint;
    if (m_field_index == 0 && !redo_last_field) {
        auto frame_buffer = m_frame_buffers.back();
        frame_buffer->set_frame_no(++m_frame_no);
        m_frame_buffers.pop_back();
        m_frame_buffers.push_front(frame_buffer);

        tie(input_vulkan_buffer, presentationHint) = m_reader.getNextInputBuffer();
        if (input_vulkan_buffer == nullptr) {
            return false;
        }

        auto eq_estimate = FrameBuffer::EstimateEq(input_vulkan_buffer->data<uint16_t>());
        if (m_eq.first == -1 && m_eq.second == -1)
            m_eq = eq_estimate;
        else
            m_eq = {m_eq.first * 0.9 + eq_estimate.first * 0.1, m_eq.second * 0.9 + eq_estimate.second * 0.1};
        if (m_frame_no % 30 == 0)
            m_log.info(eDecoder, fmt::format("eq: {}, {}", m_eq.first, m_eq.second));

        m_first_stage_command_buffer->begin();
        m_shaders.convertToFloatAndApplyEqAndGamma(*m_first_stage_command_buffer, input_vulkan_buffer, frame_buffer->data(), m_eq);
        m_shaders.convertAudioSampleRate(*m_first_stage_command_buffer, frame_buffer->data());
        m_first_stage_command_buffer->submit({}, {}, {m_first_stage_complete_semaphore});

        if (m_decode_video)
            frame_buffer->ProcessControlData(input_vulkan_buffer->data<uint16_t>(), m_eq);
    }

    // if not decoding all fields, we only decode on field 0 (when we read the data), but
    // actually decode the second field.  For field 1, the same field will be shown again.
    // Always begin the batch here since it will wait for the first stage semaphore to complete (or it won't be unsignalled)
    m_second_stage_command_buffer->begin();
    if (m_decode_video && (m_decode_all_fields || m_field_index == 0)) {
        int decoded_field_index = m_decode_all_fields ? m_field_index : 1;

        m_shaders.decodeIntraField(*m_second_stage_command_buffer, m_frame_buffers[0]->get_field(decoded_field_index));
        auto fields = vector<reference_wrapper<FieldBufferView>>{
                m_frame_buffers[0]->get_field(decoded_field_index),
                m_frame_buffers[1 - decoded_field_index]->get_field(1 - decoded_field_index),
                m_frame_buffers[1]->get_field(decoded_field_index),
                m_frame_buffers[2 - decoded_field_index]->get_field(1 - decoded_field_index),
                m_frame_buffers[2]->get_field(decoded_field_index)};

        if (m_shaders.decodeInterFrameAndDetectMotion(*m_second_stage_command_buffer, fields)) {
            m_log.debug(eDecoder | eVideo, fmt::format("Field {} inter-frame interpolation success", decoded_field_index));
            m_shaders.combineStillAndMovingParts(*m_second_stage_command_buffer,
                                                 field_interpolation_mode == eForceIntraField,
                                                 field_interpolation_mode == eForceInterFrame);
        } else {
            m_log.warn(eDecoder | eVideo, fmt::format("Field {} inter-frame interpolation failed -- using intra-field interpolation", decoded_field_index));
            m_shaders.combineStillAndMovingParts(*m_second_stage_command_buffer, /* force field only */ true, /* force inter frame only */ false);
        }
    }
    if (m_first_stage_command_buffer->isSubmitted())
        m_second_stage_command_buffer->submit({m_first_stage_complete_semaphore}, {vk::PipelineStageFlagBits::eComputeShader}, {});
    else
        m_second_stage_command_buffer->submit({}, {}, {});

    assert(input_vulkan_buffer != nullptr == m_first_stage_command_buffer->isSubmitted());
    if (m_first_stage_command_buffer->isSubmitted()) {
        m_first_stage_command_buffer->wait();
        m_reader.returnBuffer(input_vulkan_buffer);
    }

    if (m_decode_audio && m_field_index == 0)
        m_audio_decoder.decodeFrame(m_frame_no, m_shaders.getAudioData(), audio_mode, sample_count, output_samples);
    else
        sample_count = 0;

    if (m_second_stage_command_buffer->isSubmitted())
        m_second_stage_command_buffer->wait();

    if (m_timestamp_query_pool != nullptr)
        m_timestamp_statistics.add_timestamps(m_timestamp_query_pool->getTimestamps());

    auto t1 = chrono::high_resolution_clock::now();
    long time_us = chrono::duration_cast<chrono::microseconds>(t1 - t0).count();
    m_total_elapsed_time_us += time_us;
    m_log.info(ePerformance | eVideo, fmt::format("Field {} elapsed time {} ms; {} ms/frame",
                                                  m_field_index, time_us / 1000,
                                                  m_total_elapsed_time_us / 1000 / m_frame_no));

    m_field_index = (m_field_index + 1) % 2;

    return true;
}

void MuseDecoder::output_benchmark_results() {
    m_timestamp_statistics.print_stats();
}
