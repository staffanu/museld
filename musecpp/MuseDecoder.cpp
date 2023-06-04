//
// Created by staffanu on 5/22/23.
//

#include <string>
#include <netinet/in.h>
#include <iostream>
#include <map>
#include <chrono>
#include "MuseTypes.h"
#include "MuseDecoder.h"
#include "FrameBuffer.h"
#include "musevk/VulkanManager.h"

using namespace std;

MuseDecoder::MuseDecoder(
        const string &filename, Shaders &shaders, musevk::VulkanManager &manager)
: m_filename(filename),
  m_shaders(shaders),
  m_manager(manager) {
}

MuseDecoder::~MuseDecoder() {
    while (!m_frame_buffers.empty()) {
        delete m_frame_buffers.back();
        m_frame_buffers.pop_back();
    }
}

bool MuseDecoder::Initialize() {
    auto [ samples_to_skip, eq ] = compute_initial_skip();
    m_eq = eq;

    m_input = ifstream(static_cast<string>(m_filename).c_str(), ios::binary | ios::in);
    m_input.exceptions(ifstream::badbit);

    {
        vector<uint16_t> skip_buffer(samples_to_skip);
        cout << "Skipping " << samples_to_skip << " initial samples" << endl;
        read_shorts(m_input, skip_buffer.data(), samples_to_skip);
    }

    m_input_vulkan_buffer = m_manager.createBuffer(MUSE_TOTAL_HEIGHT * MUSE_TOTAL_WIDTH,sizeof(uint16_t), true, true);

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

bool MuseDecoder::Next() {
    auto t0 = chrono::high_resolution_clock::now();
    auto sq = m_manager.sequence();
    if (m_field_index == 0) {
        auto frame_buffer = m_frame_buffers.back();
        frame_buffer->set_frame_no(++m_frame_no);
        m_frame_buffers.pop_back();
        m_frame_buffers.push_front(frame_buffer);

        if (!read_shorts(m_input, m_input_vulkan_buffer->data<uint16_t>(), 480 * 1125))
            return false;
        //sq->enqueueSyncDevice(m_input_vulkan_buffer);

        auto eq_estimate = FrameBuffer::EstimateEq(m_input_vulkan_buffer->data<uint16_t>());
        m_eq = {m_eq.first * 0.9 + eq_estimate.first * 0.1, m_eq.second * 0.9 + eq_estimate.second * 0.1};
        cout << "eq: " << m_eq.first << ", " << m_eq.second << endl;

        frame_buffer->ProcessControlData(m_input_vulkan_buffer->data<uint16_t>(), m_eq);

        m_shaders.convertToFloatAndApplyEqAndGamma(*sq, m_input_vulkan_buffer, frame_buffer->data(), m_eq);
    }

    m_shaders.decodeIntraField(*sq, m_frame_buffers[0]->get_field(m_field_index));
    auto fields = vector<reference_wrapper<FieldBufferView>>{
            m_frame_buffers[0]->get_field(m_field_index),
            m_frame_buffers[1 - m_field_index]->get_field(1 - m_field_index),
            m_frame_buffers[1]->get_field(m_field_index),
            m_frame_buffers[2 - m_field_index]->get_field(1 - m_field_index),
            m_frame_buffers[2]->get_field(m_field_index)};

    if (m_shaders.decodeInterFrameAndDetectMotion(*sq, fields)) {
        cout << "Field " << m_field_index << " inter-frame interpolation success" << endl;
        m_shaders.combineStillAndMovingParts(*sq, false, false);
    } else {
        cout << "Field " << m_field_index << " inter-frame interpolation failed -- using intra-field interpolation" << endl;
        m_shaders.combineStillAndMovingParts(*sq, true, false);
    }
    sq->evalAsync();

    // FIXME: the frame buffer is not synched local here!
    m_audio_decoder.decode_field(m_frame_buffers[0]->get_field(m_field_index).audio_buffer());
    sq->evalAwait();

    auto t1 = chrono::high_resolution_clock::now();
    long time_us = chrono::duration_cast<chrono::microseconds>(t1 - t0).count();
    m_total_elapsed_time_us += time_us;
    cout << "Field elapsed time " << time_us / 1000 << " ms; " << (m_total_elapsed_time_us / 1000 / m_frame_no) << " ms/frame" << endl;

    m_field_index = (m_field_index + 1) % 2;

    return true;
}

bool MuseDecoder::read_big_endian_to_buffer(ifstream &input, float *out, size_t n) {
    uint16_t *input_buffer = (uint16_t *)malloc(480 * 1125 * 2 * sizeof(float));
    input.read(reinterpret_cast<char *>(input_buffer), n * sizeof(uint16_t));
    for (int i = 0; i < n; i++) {
        out[i] = (float)ntohs(input_buffer[i]) / MUSE_SHORT_INPUT_MULT;
    }
    return input.good();
}

bool MuseDecoder::read_shorts(ifstream &input, uint16_t *out, size_t n) {
    input.read(reinterpret_cast<char *>(out), n * sizeof(uint16_t));
    return input.good();
}

pair<int, pair<float, float>> MuseDecoder::compute_initial_skip() {
    ifstream input(static_cast<string>(m_filename).c_str(), ios::binary | ios::in);
    input.exceptions(ifstream::badbit);
    vector<float> buffer(480 * 1125 * 2); // two frames of data
    read_big_endian_to_buffer(input, buffer.data(), 480 * 1125 * 2);

    auto sorted(buffer);
    sort(sorted.begin(), sorted.end());
    auto y1 = sorted[500];
    auto y2 = sorted[480 * 1125 * 2 - 500];
    pair<float, float> eq = {(y2 - y1) / (239.0f - 16.0f), y1 - 16.0f};
    cout << "Initial eq: " << eq.first << ", " << eq.second << endl;

    auto equalized(buffer);
    for (float &it: equalized)
        it = ((it - eq.second) / eq.first);

    // checks if the line starting at off has a positive or a negative sync
    auto isSyncGood = [&equalized](int off, bool expectedPositive) {
        bool isPos = equalized[off + 3] < equalized[off + 5] && equalized[off + 5] < equalized[off + 7];
        bool isNeg = equalized[off + 3] > equalized[off + 5] && equalized[off + 5] > equalized[off + 7];
        return expectedPositive && isPos || !expectedPositive && isNeg;
    };

    auto goodSynchsForPixelOffsets = map<int, int>();
    for (int pixelOffset = 0; pixelOffset < 480; pixelOffset++) {
        int goodSynchsForLineStarts = 0;
        for (int startLine = 0; startLine < 2000; startLine += 50) {
            int goodSynchsPerPhase = 0;
            for (int phase = 0; phase <= 1; phase++) {
                int goodSyncs = 0;
                for (int line = startLine; line < startLine + 50; line++) {
                    int off = line * 480 + pixelOffset;
                    if (isSyncGood(off, (line + phase) % 2 == 0))
                        goodSyncs++;
                }
                if (goodSyncs > 47)
                    goodSynchsPerPhase++;
            }
            if (goodSynchsPerPhase != 0)
                goodSynchsForLineStarts++;
        }
        goodSynchsForPixelOffsets[pixelOffset] = goodSynchsForLineStarts;
    }
    int bestPixelOffset = max_element(goodSynchsForPixelOffsets.cbegin(), goodSynchsForPixelOffsets.cend(),
                                      [] (const pair<int, int> & p1, const pair<int, int> & p2) {
                                          return p1.second < p2.second;
                                      })->first;

    auto goodnessForLineOffset = map<int, int>();
    for (int lineOffset = 0; lineOffset < 1125; lineOffset++) {
        int goodSyncsForOffset = 0;
        for (int line = 0; line < 1125; line++) {
            int expectedPosSync = line == 0 || line > 2 && line % 2 == 1;
            if (isSyncGood((lineOffset + line) * 480 + bestPixelOffset, expectedPosSync))
                goodSyncsForOffset++;
        }
        goodnessForLineOffset[lineOffset] = goodSyncsForOffset;
    }
    int bestLineOffset = max_element(goodnessForLineOffset.cbegin(), goodnessForLineOffset.cend(),
                                     [] (const pair<int, int> & p1, const pair<int, int> & p2) {
                                         return p1.second < p2.second;
                                     })->first;

    input.close();

    return { bestLineOffset * 480 + bestPixelOffset, eq };
}
