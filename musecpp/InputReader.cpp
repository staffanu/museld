//
// Created by staffanu on 6/25/23.
//

#include <vector>
#include <netinet/in.h>
#include <algorithm>
#include <map>
#include <iostream>
#include <unistd.h>
#include <fcntl.h>
#include "InputReader.h"
#include "MuseTypes.h"

using namespace std;

bool InputReader::initialize() {
//    auto [samples_to_skip, eq] = compute_initial_skip();

//    m_input = ifstream(static_cast<string>(m_filename).c_str(), ios::binary | ios::in);
//    m_input.exceptions(ifstream::badbit);

//    for (int i = 0; i < c_interpolation_buffer_size; i++)
//        m_interpolation_buffer.push_back(0);

//    vector<uint16_t> skip_buffer(samples_to_skip);
//    cout << "Skipping " << samples_to_skip << " initial samples" << endl;
//    readShorts(m_input, skip_buffer.data(), samples_to_skip);

    m_file_fd = open(m_filename.c_str(), 0);
    if (m_file_fd == -1)
        throw runtime_error("Unable to open file");

    // cout << "Seeking: " << lseek(m_file_fd, 500000 * 3000, SEEK_SET) << endl;

    return true;
}

bool InputReader::readBigEndianToBuffer(ifstream &input, float *out, size_t n) {
    uint16_t *input_buffer = (uint16_t *)malloc(480 * 1125 * 2 * sizeof(float));
    input.read(reinterpret_cast<char *>(input_buffer), n * sizeof(uint16_t));
    for (int i = 0; i < n; i++) {
        out[i] = (float)ntohs(input_buffer[i]) / MUSE_SHORT_INPUT_MULT;
    }
    return input.good();
}

InputReader::~InputReader() {
    if (m_file_fd != -1)
        close(m_file_fd);
}

bool InputReader::readShorts(ifstream &input, uint16_t *out, size_t n) {
    input.read(reinterpret_cast<char *>(out), n * sizeof(uint16_t));
    return input.good();
}

pair<int, pair<float, float>> InputReader::compute_initial_skip() {
    ifstream input(static_cast<string>(m_filename).c_str(), ios::binary | ios::in);
    input.exceptions(ifstream::badbit);
    vector<float> buffer(480 * 1125 * 2); // two frames of data
    readBigEndianToBuffer(input, buffer.data(), 480 * 1125 * 2);

    auto sorted(buffer);
    sort(sorted.begin(), sorted.end());
    auto y1 = sorted[500];
    auto y2 = sorted[480 * 1125 * 2 - 500];
    pair<float, float> eq = {(y2 - y1) / (239.0f - 16.0f), y1 - 16.0f};
    cout << "Initial eq: " << eq.first << ", " << eq.second << endl;

    auto equalized(buffer);
    for (float &it: equalized)
        it = ((it - eq.second) / eq.first);

    // checks if the m_line starting at off has a positive or a negative sync
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

bool InputReader::readShorts(uint16_t *buffer) {
    uint16_t sample_buffer[c_sample_buffer_size];
    InputPll::PllResult pll_result{};
    double input_samples_per_sample = m_input_pll.getInputSamplesPerSample();
    int samples_to_read = 1;
    while (readSamples(samples_to_read, sample_buffer, input_samples_per_sample)) {
        pll_result = m_input_pll.process(samples_to_read, sample_buffer, buffer);
        samples_to_read = pll_result.samples_to_read;
        input_samples_per_sample = pll_result.input_samples_per_sample;
        if (pll_result.frame_done)
            break;
    }
    return pll_result.frame_done;
}

bool InputReader::readSamples(int sample_count, uint16_t buffer[c_sample_buffer_size], double dt) {
    double t = m_t;
    for (int sample_ix = 0; sample_ix < sample_count; sample_ix++) {
        double bytes_read = floor(t);
        t += dt;
        double t1_int = floor(t);
        while (bytes_read < t1_int) {
            while (m_input_buffer_read_pos >= m_input_buffer_bytes) {
                m_input_buffer_bytes = read(m_file_fd, (void *) m_input_buffer, c_input_buffer_size);
                if (m_input_buffer_bytes == -1)
                    throw runtime_error("Error reading from file");
                else if (m_input_buffer_bytes == 0 && m_stop_on_eof) {
                    return false;
                }
                m_input_buffer_read_pos = 0;
            }
            uint8_t b = m_input_buffer[m_input_buffer_read_pos++];
            bytes_read++;

            m_last_written_ix++;
            if (m_last_written_ix == c_interpolation_buffer_size)
                m_last_written_ix = 0;
            m_interpolation_buffer[m_last_written_ix] = b;
        }
        t -= t1_int;

        double b0 = (m_interpolation_buffer[(m_last_written_ix + c_interpolation_buffer_size - 3) % c_interpolation_buffer_size] & 0xff);
        double b1 = (m_interpolation_buffer[(m_last_written_ix + c_interpolation_buffer_size - 2) % c_interpolation_buffer_size] & 0xff);
        double b2 = (m_interpolation_buffer[(m_last_written_ix + c_interpolation_buffer_size - 1) % c_interpolation_buffer_size] & 0xff);
        double b3 = (m_interpolation_buffer[m_last_written_ix] & 0xff);

        double x = 1 + t;
        // cubic spline though 4 points, f(x) = a0 + a1 x + a2 x(x-1) + a3 x(x-1)(x-2)
        double a0 = b0;
        double a1 = b1 - a0;
        double a2 = (b2 - a0 - 2 * a1) / 2;
        double a3 = (b3 - a0 - 3 * a1 - 6 * a2) / 6;
        // now evaluate at point 1 + p
        double y = a0 + a1 * x + a2 * x * (x - 1) + a3 * x * (x - 1) * (x - 2);

        buffer[sample_ix] = (uint16_t)(y * MUSE_SHORT_INPUT_MULT);
    }
    m_t = t;
    return true;
}