#include <iostream>
#include <vector>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <map>
#include <netinet/in.h>
#include <deque>
// #include <vlc/vlc.h>
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include "Eigen/Dense"
#include "MuseTypes.h"
#include "FrameBuffer.h"
#include "VideoDecoder.h"
#include "AudioDecoder.h"

using namespace Eigen;
using namespace std;

bool read_shorts_big_endian_to_buffer(ifstream &input, uint16_t *input_buffer, uint8_t *out, size_t n, pair<double, double> eq) {
    input.read(reinterpret_cast<char *>(input_buffer), n * sizeof(uint16_t));
    for (int i = 0; i < n; i++) {
        uint16_t v = ntohs(input_buffer[i]);
        uint16_t equalized_v = (uint8_t)clamp(((double)v / MUSE_INPUT_MULT - eq.second) / eq.first,
                                                 0.0, 255.0);
        out[i] = equalized_v;
    }
    return input.good();
}

pair<int, pair<double, double>> compute_initial_skip(string_view filename) {
    ifstream input(static_cast<string>(filename).c_str(), ios::binary | ios::in);
    input.exceptions(ifstream::badbit);
    uint16_t input_buffer[480 * 1125 * 2 * sizeof(uint16_t)];
    vector<uint8_t> buffer(480 * 1125 * 2); // two frames of data
    read_shorts_big_endian_to_buffer(input, input_buffer, buffer.data(), 480 * 1125 * 2, pair(1.0, 0.0));

    auto sorted(buffer);
    sort(sorted.begin(), sorted.end());
    auto y1 = (int)sorted[500];
    auto y2 = (int)sorted[480 * 1125 * 2 - 500];
    pair<double, double> eq = {(y2 - y1) / (239.0 - 16.0), y1 - 16.0};
    cout << "Initial eq: " << eq.first << ", " << eq.second << endl;

    auto equalized(buffer);
    for (uint8_t &it: equalized)
        it = (uint8_t)(((double) it - eq.second) / eq.first);

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

void process_file(string_view filename) {
    auto [ samples_to_skip, eq ] = compute_initial_skip(filename);

    ifstream input(static_cast<string>(filename).c_str(), ios::binary | ios::in);
    input.exceptions(ifstream::badbit);
    uint16_t input_buffer[480 * 1125 * sizeof(uint16_t)];

    {
        vector<uint8_t> skip_buffer(samples_to_skip);
        cout << "Skipping " << samples_to_skip << " initial samples" << endl;
        read_shorts_big_endian_to_buffer(input, input_buffer, skip_buffer.data(), samples_to_skip, pair(1.0, 0.0));
    }

    auto audio_decoder = AudioDecoder();

    deque<FrameBuffer *> frame_buffers;

    int frameNo = 0;
    auto t0 = chrono::high_resolution_clock::now();
    uint8_t *frame_mem;
    while (frame_mem = new uint8_t[1125 * 480],
            read_shorts_big_endian_to_buffer(input, input_buffer, frame_mem, 480 * 1125, eq)) {
        auto *frame_buffer = new FrameBuffer(++frameNo, frame_mem);

        // Update control data from the previous fields
        if (!frame_buffers.empty()) {
            auto prev_frame = frame_buffers.front();
            auto prev_control_data = prev_frame->get_field(1).control_data_buffer();
            frame_buffer->get_field(0).ProcessControlData(prev_control_data);
        }
        {
            auto field0_control_data = frame_buffer->get_field(0).control_data_buffer();
            frame_buffer->get_field(1).ProcessControlData(field0_control_data);
        }

        // Keep the three latest frames (required for motion detection)
        frame_buffers.push_front(frame_buffer);
        if (frame_buffers.size() > 3) {
            delete frame_buffers.back();
            frame_buffers.pop_back();
        }

        auto eq_estimate = frame_buffer->estimate_eq();
        eq = { eq.first * 0.9 + eq_estimate.first * 0.1, eq.second * 0.9 + eq_estimate.second * 0.1 };
        cout << "eq: " << eq.first << ", " << eq.second << endl;

        frame_buffer->ApplyInverseTransmissionGamma();

        cv::Mat fieldMat(MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3, CV_8UC3);

        auto view0 = frame_buffers.front()->get_field(0);
        auto view1 = frame_buffers.front()->get_field(1);

        VideoDecoder::DecodeSingleField(view0, fieldMat);
        cout << "Field 0 display" << endl;
        cv::imshow("MUSE", fieldMat);
        cv::waitKey(1);

        VideoDecoder::DecodeSingleField(view1, fieldMat);
        cout << "Field 1 display" << endl;
        cv::imshow("MUSE", fieldMat);
        cv::waitKey(1);

        audio_decoder.decode_field(frame_buffer->get_field(0).audio_buffer());
        audio_decoder.decode_field(frame_buffer->get_field(1).audio_buffer());

        auto t1 = chrono::high_resolution_clock::now();
        long time_us = chrono::duration_cast<chrono::microseconds>(t1 - t0).count();
        cout << "Elapsed time " << time_us / 1000 << " ms; " << (time_us / 1000 / frameNo) << " ms/frame" << endl;
    }

    while (!frame_buffers.empty()) {
        delete frame_buffers.back();
        frame_buffers.pop_back();
    }
}

int main(int argc, char *argv[]) {
    try {
        const vector<string_view> args(argv + 1, argv + argc);
        for (auto it = args.cbegin(), end = args.cend(); it != end; ++it) {
            if (!filesystem::exists(*it)) {
                throw runtime_error("File not found: " + string(*it));
            }
            process_file(*it);
        }
    } catch (const exception &x) {
        cerr << "musecpp: " << x.what() << '\n';
        cerr << "usage: musecpp <input_file> ...\n";
        return EXIT_FAILURE;
    }

    return 0;
}
