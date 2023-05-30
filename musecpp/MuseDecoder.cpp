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

using namespace std;

MuseDecoder::MuseDecoder(const string &filename, bool read_floats, Shaders &shaders)
: m_filename(filename),
  m_read_floats(read_floats),
  m_input_buffer((uint16_t *)malloc(480 * 1125 * 2 * sizeof(float))), // big enough for floats or shorts, two frames total
  m_shaders(shaders) {
}

MuseDecoder::~MuseDecoder() {
    while (!m_frame_buffers.empty()) {
        delete m_frame_buffers.back();
        m_frame_buffers.pop_back();
    }
    delete m_input_buffer;
}

bool MuseDecoder::Initialize() {
    auto [ samples_to_skip, eq ] = compute_initial_skip();
    m_eq = eq;

    m_input = ifstream(static_cast<string>(m_filename).c_str(), ios::binary | ios::in);
    m_input.exceptions(ifstream::badbit);

    {
        vector<float> skip_buffer(samples_to_skip);
        cout << "Skipping " << samples_to_skip << " initial samples" << endl;
        read_big_endian_to_buffer(m_input, skip_buffer.data(), samples_to_skip, pair(1.0, 0.0));
    }

    m_frame_no = 0;
    m_field_index = 0;
    m_total_elapsed_time_us = 0;

    return true;
}

bool MuseDecoder::Next() {
    auto t0 = chrono::high_resolution_clock::now();
    if (m_field_index == 0) {
        auto *frame_mem = new float[1125 * 480];
        if (!read_big_endian_to_buffer(m_input, frame_mem, 480 * 1125, m_eq))
            return false;
        auto frame_tensor = m_shaders.resources().tensor(frame_mem, MUSE_TOTAL_HEIGHT * MUSE_TOTAL_WIDTH, sizeof(float));
        auto muse_buffer = make_shared<MuseBuffer<float>>(MUSE_TOTAL_HEIGHT, MUSE_TOTAL_WIDTH, frame_tensor);
        delete[] frame_mem;
        auto *frame_buffer = new FrameBuffer(++m_frame_no, muse_buffer);

        // Update control data from the previous fields
        if (!m_frame_buffers.empty()) {
            auto prev_frame = m_frame_buffers.front();
            auto prev_control_data = prev_frame->get_field(1).control_data_buffer();
            frame_buffer->get_field(0).ProcessControlData(prev_control_data);
        }
        {
            auto field0_control_data = frame_buffer->get_field(0).control_data_buffer();
            frame_buffer->get_field(1).ProcessControlData(field0_control_data);
        }

        // Keep the three latest frames (required for motion detection)
        m_frame_buffers.push_front(frame_buffer);
        if (m_frame_buffers.size() > 3) {
            delete m_frame_buffers.back();
            m_frame_buffers.pop_back();
        }

        auto eq_estimate = frame_buffer->estimate_eq(m_eq);
        m_eq = {m_eq.first * 0.9 + eq_estimate.first * 0.1, m_eq.second * 0.9 + eq_estimate.second * 0.1};
        cout << "eq: " << m_eq.first << ", " << m_eq.second << endl;

        m_shaders.ApplyTransmissionGamma(*frame_buffer->data());
    }

    bool inter_frame_ok = false;
    m_shaders.DecodeIntraField(m_frame_buffers[0]->get_field(m_field_index));
    if (m_frame_buffers.size() >= 3) {
        auto fields = vector<reference_wrapper<FieldBufferView>>{
                m_frame_buffers[0]->get_field(m_field_index),
                m_frame_buffers[1 - m_field_index]->get_field(1 - m_field_index),
                m_frame_buffers[1]->get_field(m_field_index),
                m_frame_buffers[2 - m_field_index]->get_field(1 - m_field_index),
                m_frame_buffers[2]->get_field(m_field_index)};

        if (m_shaders.DecodeInterFrameAndDetectMotion(fields)) {
            inter_frame_ok = true;
            m_shaders.CombineStillAndMovingParts(false, false);
            cout << "Field " << m_field_index << " inter-frame interpolation success" << endl;
        } else {
            cout << "Field " << m_field_index << " inter-frame interpolation failed!" << endl;
        }
    }
    if (!inter_frame_ok) {
        m_shaders.CombineStillAndMovingParts(true, false);
        cout << "Field " << m_field_index << " using intra-field interpolation" << endl;
    }

    m_audio_decoder.decode_field(m_frame_buffers[0]->get_field(m_field_index).audio_buffer());

    auto t1 = chrono::high_resolution_clock::now();
    long time_us = chrono::duration_cast<chrono::microseconds>(t1 - t0).count();
    m_total_elapsed_time_us += time_us;
    cout << "Field elapsed time " << time_us / 1000 << " ms; " << (m_total_elapsed_time_us / 1000 / m_frame_no) << " ms/frame" << endl;

    m_field_index = (m_field_index + 1) % 2;

    return true;
}

bool MuseDecoder::read_big_endian_to_buffer(ifstream &input, float *out, size_t n, pair<float, float> eq) {
    input.read(reinterpret_cast<char *>(m_input_buffer), n * (m_read_floats ? sizeof(uint32_t) : sizeof(uint16_t)));
    for (int i = 0; i < n; i++) {
        float v;
        if (m_read_floats) {
            uint32_t v_long = ntohl(((uint32_t *)m_input_buffer)[i]);
            v = *reinterpret_cast<float *>(&v_long) / MUSE_FLOAT_INPUT_MULT;
        } else {
            v = (float)ntohs(m_input_buffer[i]) / MUSE_SHORT_INPUT_MULT;
        }

        float equalized_v = clamp((v - eq.second) / eq.first, 0.0f, 255.0f);
        out[i] = equalized_v;
    }
    return input.good();
}

pair<int, pair<float, float>> MuseDecoder::compute_initial_skip() {
    ifstream input(static_cast<string>(m_filename).c_str(), ios::binary | ios::in);
    input.exceptions(ifstream::badbit);
    vector<float> buffer(480 * 1125 * 2); // two frames of data
    read_big_endian_to_buffer(input, buffer.data(), 480 * 1125 * 2, pair(1.0, 0.0));

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
