//
// Created by staffanu on 5/22/23.
//

#include <string>
#include <iostream>
#include <map>
#include <chrono>
#include "MuseTypes.h"
#include "MuseDecoder.h"
#include "FrameBuffer.h"
#include "musevk/VulkanManager.h"

using namespace std;

MuseDecoder::MuseDecoder(
        InputReader &reader, Shaders &shaders, musevk::VulkanManager &manager,
        bool decode_video, bool decode_all_fields, bool decode_audio, bool benchmark_shaders)
: m_reader(reader),
  m_shaders(shaders),
  m_manager(manager),
  m_decode_video(decode_video),
  m_decode_all_fields(decode_all_fields),
  m_decode_audio(decode_audio),
  m_benchmark_shaders(benchmark_shaders),
  m_eq{-1, -1},
  m_command_queue(m_manager.createCommandQueue({}, {}, benchmark_shaders ? 40 : 0)) {
}

MuseDecoder::~MuseDecoder() {
    while (!m_frame_buffers.empty()) {
        delete m_frame_buffers.back();
        m_frame_buffers.pop_back();
    }
}

bool MuseDecoder::initialize() {
    m_input_vulkan_buffer = m_manager.createBuffer(MUSE_TOTAL_HEIGHT * MUSE_TOTAL_WIDTH, sizeof(uint16_t), true, true);

    // Always keep the three latest frames (required for motion detection) -- pretend we have three already
    for (int i = 0; i < 3; i++)
        m_frame_buffers.push_back(new FrameBuffer(-i,
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
                       AudioDecoder::AudioFrame output_samples[AudioDecoder::c_max_output_samples]) {
    auto t0 = chrono::high_resolution_clock::now();
    if (m_field_index == 0) {
        auto frame_buffer = m_frame_buffers.back();
        frame_buffer->set_frame_no(++m_frame_no);
        m_frame_buffers.pop_back();
        m_frame_buffers.push_front(frame_buffer);

        if (!m_reader.readShorts(m_input_vulkan_buffer->data<uint16_t>())) {
            m_timestamp_statistics.print_stats();
            return false;
        }

        auto eq_estimate = FrameBuffer::EstimateEq(m_input_vulkan_buffer->data<uint16_t>());
        if (m_eq.first == -1 && m_eq.second == -1)
            m_eq = eq_estimate;
        else
            m_eq = {m_eq.first * 0.9 + eq_estimate.first * 0.1, m_eq.second * 0.9 + eq_estimate.second * 0.1};
        cout << "eq: " << m_eq.first << ", " << m_eq.second << endl;

        // Since we really only need to decode the control data when we process the second field,
        // we should probably delay this until graphics operations are queued. But it is quick.
        if (m_decode_video)
            frame_buffer->ProcessControlData(m_input_vulkan_buffer->data<uint16_t>(), m_eq);

        m_shaders.convertToFloatAndApplyEqAndGamma(*m_command_queue, m_input_vulkan_buffer, frame_buffer->data(), m_eq);

        m_shaders.convertAudioSampleRate(*m_command_queue, frame_buffer->data());
    }

    if (m_decode_video) {
        int decoded_field_index = m_decode_all_fields ? m_field_index : 1;

        m_shaders.decodeIntraField(*m_command_queue, m_frame_buffers[0]->get_field(decoded_field_index));
        auto fields = vector<reference_wrapper<FieldBufferView>>{
                m_frame_buffers[0]->get_field(decoded_field_index),
                m_frame_buffers[1 - decoded_field_index]->get_field(1 - decoded_field_index),
                m_frame_buffers[1]->get_field(decoded_field_index),
                m_frame_buffers[2 - decoded_field_index]->get_field(1 - decoded_field_index),
                m_frame_buffers[2]->get_field(decoded_field_index)};

        if (m_shaders.decodeInterFrameAndDetectMotion(*m_command_queue, fields)) {
            cout << "Field " << decoded_field_index << " inter-frame interpolation success" << endl;
            m_shaders.combineStillAndMovingParts(*m_command_queue, false, false);
        } else {
            cout << "Field " << decoded_field_index << " inter-frame interpolation failed -- using intra-field interpolation"
                 << endl;
            m_shaders.combineStillAndMovingParts(*m_command_queue, true, false);
        }
    }
    if (m_field_index == 0 || m_decode_video) {
        m_command_queue->evalAsync();
        m_command_queue->evalAwait(); // FIXME: remove and add fence
    }
    if (m_decode_audio && m_field_index == 0)
        m_audio_decoder.decodeFrame(m_shaders.getAudioData(), audio_mode, sample_count, output_samples);
    else
        sample_count = 0;
    //m_command_queue->evalAwait();

    if (m_benchmark_shaders)
        m_timestamp_statistics.add_timestamps(m_command_queue->getTimestamps());

    auto t1 = chrono::high_resolution_clock::now();
    long time_us = chrono::duration_cast<chrono::microseconds>(t1 - t0).count();
    m_total_elapsed_time_us += time_us;
    cout << "Field elapsed time " << time_us / 1000 << " ms; " << (m_total_elapsed_time_us / 1000 / m_frame_no) << " ms/frame" << endl;

    m_field_index = m_decode_all_fields ? (m_field_index + 1) % 2 : 0;

    return true;
}
