#include <iostream>
#include <memory>
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
#include "MuseTypes.h"
#include "FrameBuffer.h"
#include "FieldBufferView.h"
#include "AudioDecoder.h"
#include "Shaders.h"

using namespace Eigen;
using namespace std;

// big enough for floats or shorts, two frames total
static uint16_t input_buffer[480 * 1125 * 2 * sizeof(uint32_t)];

bool read_big_endian_to_buffer(ifstream &input, bool read_floats, float *out, size_t n, pair<float, float> eq) {
    input.read(reinterpret_cast<char *>(input_buffer), n * (read_floats? sizeof(uint32_t) : sizeof(uint16_t)));
    for (int i = 0; i < n; i++) {
        float v;
        if (read_floats) {
            uint32_t v_long = ntohl(((uint32_t *)input_buffer)[i]);
            v = *reinterpret_cast<float *>(&v_long) / MUSE_FLOAT_INPUT_MULT;
        } else {
            v = (float)ntohs(input_buffer[i]) / MUSE_SHORT_INPUT_MULT;
        }

        float equalized_v = clamp((v - eq.second) / eq.first, 0.0f, 255.0f);
        out[i] = equalized_v;
    }
    return input.good();
}

pair<int, pair<float, float>> compute_initial_skip(string_view filename, bool read_floats) {
    ifstream input(static_cast<string>(filename).c_str(), ios::binary | ios::in);
    input.exceptions(ifstream::badbit);
    vector<float> buffer(480 * 1125 * 2); // two frames of data
    read_big_endian_to_buffer(input, read_floats, buffer.data(), 480 * 1125 * 2, pair(1.0, 0.0));

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

void process_file(string_view filename, bool read_floats) {
    auto [ samples_to_skip, eq ] = compute_initial_skip(filename, read_floats);

    ifstream input(static_cast<string>(filename).c_str(), ios::binary | ios::in);
    input.exceptions(ifstream::badbit);

    {
        vector<float> skip_buffer(samples_to_skip);
        cout << "Skipping " << samples_to_skip << " initial samples" << endl;
        read_big_endian_to_buffer(input, read_floats, skip_buffer.data(), samples_to_skip, pair(1.0, 0.0));
    }

    Shaders::CreateInstance();
    Shaders &shaders = Shaders::GetInstance();
    auto audio_decoder = AudioDecoder();

    deque<FrameBuffer *> frame_buffers;

    int frameNo = 0;
    auto t0 = chrono::high_resolution_clock::now();
    float *frame_mem;
    while (frame_mem = new float[1125 * 480],
            read_big_endian_to_buffer(input, read_floats, frame_mem, 480 * 1125, eq)) {
        auto frame_tensor = Shaders::GetManager().tensor(frame_mem, MUSE_TOTAL_HEIGHT * MUSE_TOTAL_WIDTH,
                                                         sizeof(float), kp::Tensor::TensorDataTypes::eFloat);
        auto muse_buffer = make_shared<MuseBuffer<float>>(MUSE_TOTAL_HEIGHT, MUSE_TOTAL_WIDTH, frame_tensor);
        delete[] frame_mem;
        auto *frame_buffer = new FrameBuffer(++frameNo, muse_buffer);

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

        auto eq_estimate = frame_buffer->estimate_eq(eq);
        eq = { eq.first * 0.9 + eq_estimate.first * 0.1, eq.second * 0.9 + eq_estimate.second * 0.1 };
        cout << "eq: " << eq.first << ", " << eq.second << endl;

        frame_buffer->ApplyInverseTransmissionGamma();

#define WAITKEY_ARG 0
        auto view0 = frame_buffers.front()->get_field(0);
        auto view1 = frame_buffers.front()->get_field(1);

        if (frame_buffers.size() >= 3) {
        cv::namedWindow("MUSE", cv::WINDOW_NORMAL | cv::WINDOW_FREERATIO);
        cv::resizeWindow("MUSE", 16 * 100, 9 * 100);

        auto fieldMat = shaders.DecodeIntraField(view0);
        cout << "Field 0 display" << endl;
        cv::imshow("MUSE", fieldMat);
        cv::waitKey(WAITKEY_ARG);

            auto fields0 = vector<reference_wrapper<FieldBufferView>>{
                    frame_buffers[0]->get_field(0),
                    frame_buffers[1]->get_field(1),
                    frame_buffers[1]->get_field(0),
                    frame_buffers[2]->get_field(1),
                    frame_buffers[2]->get_field(0)};
            auto interFrameMat = shaders.DecodeInterFrame(fields0);
            if (interFrameMat.has_value()) {
                cout << "Field 0 inter frame interpolated display" << endl;
                cv::imshow("MUSE", interFrameMat.value());
                cv::waitKey(WAITKEY_ARG);

                auto movementMat = shaders.DetectMotion(fields0);
                if (movementMat.has_value()) {
//                  cv::imshow("MUSE", movementMat.value());
//                  cv::waitKey(0);
                    auto combinedResult = shaders.CombineStillAndMovingParts();
                    cout << "Field 0 combined display" << endl;
                    cv::imshow("MUSE", combinedResult.value());
                    cv::waitKey(WAITKEY_ARG);
                }
            } else
                cout << "Field 0 inter frame interpolation failed!" << endl;
        }

        if (frame_buffers.size() >= 3) {
        auto fieldMat = shaders.DecodeIntraField(view1);
        cout << "Field 1 display" << endl;
        cv::imshow("MUSE", fieldMat);
        cv::waitKey(WAITKEY_ARG);

            auto fields1 = vector<reference_wrapper<FieldBufferView>>{
                    frame_buffers[0]->get_field(1),
                    frame_buffers[0]->get_field(0),
                    frame_buffers[1]->get_field(1),
                    frame_buffers[1]->get_field(0),
                    frame_buffers[2]->get_field(1)};
            auto interFrameMat = shaders.DecodeInterFrame(fields1);
            if (interFrameMat.has_value()) {
                cout << "Field 1 inter frame interpolated display" << endl;
                cv::imshow("MUSE", interFrameMat.value());
                cv::waitKey(WAITKEY_ARG);

                auto movementMat = shaders.DetectMotion(fields1);
                if (movementMat.has_value()) {
//                  cv::imshow("MUSE", movementMat.value());
//                  cv::waitKey(0);
                    auto combinedResult = shaders.CombineStillAndMovingParts();
                    cout << "Field 1 combined display" << endl;
                    cv::imshow("MUSE", combinedResult.value());
                    cv::waitKey(WAITKEY_ARG);
                }
            } else
                cout << "Field 1 inter frame interpolation failed!" << endl;
        }

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
        bool read_floats = false;
        const vector<string_view> args(argv + 1, argv + argc);
        for (auto it = args.cbegin(), end = args.cend(); it != end; ++it) {
            if (*it == "-f")
                read_floats = true;
            else if (*it == "-s")
                read_floats = false;
            else {
                if (!filesystem::exists(*it))
                    throw runtime_error("File not found: " + string(*it));
                process_file(*it, read_floats);
            }
        }
    } catch (const exception &x) {
        cerr << "musecpp: " << x.what() << '\n';
        cerr << "usage: musecpp [-f] [-s] <input_file> ...\n";
        return EXIT_FAILURE;
    }

    return 0;
}
