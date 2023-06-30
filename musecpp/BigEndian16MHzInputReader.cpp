//
// Created by staffanu on 6/30/23.
//

#include <netinet/in.h>
#include <map>
#include "MuseTypes.h"
#include "BigEndian16MHzInputReader.h"

using namespace std;

BigEndian16MHzInputReader::BigEndian16MHzInputReader(
        const std::string &filename, bool stop_on_eof)
        : InputReader(filename, stop_on_eof),
        m_input{} {
}

bool BigEndian16MHzInputReader::initialize(std::vector<std::shared_ptr<musevk::VulkanBuffer>> const &buffers) {
    auto [samples_to_skip, eq] = compute_initial_skip();

    m_input = ifstream(static_cast<string>(m_filename).c_str(), ios::binary | ios::in);
    m_input.exceptions(ifstream::badbit);

    vector<uint16_t> skip_buffer(samples_to_skip);
    cout << "Skipping " << samples_to_skip << " initial samples" << endl;
    readShorts(m_input, skip_buffer.data(), samples_to_skip);

    return InputReader::initialize(buffers);
}

void BigEndian16MHzInputReader::cleanup() {
    InputReader::cleanup();
    m_input.close();
}

void BigEndian16MHzInputReader::threadFunc() {
    //pthread_setname_np(m_reader_thread->native_handle(), "musecpp-reader");

    for (;;) {
        shared_ptr<musevk::VulkanBuffer> buffer = nullptr;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv_vacant.wait(lock, [this]{return m_stop_request || !m_vacant_muse_input_buffers.empty();});
            if (m_stop_request)
                break;
            buffer = m_vacant_muse_input_buffers.front();
            m_vacant_muse_input_buffers.pop_front();
        }

        auto data = buffer->data<uint16_t>();
        if (!readShorts(m_input, data, MUSE_TOTAL_HEIGHT * MUSE_TOTAL_WIDTH))
            break;

        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv_filled.notify_one();
        m_filled_muse_input_buffers.push_back(buffer);
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv_filled.notify_one();
    m_reader_thread_finished = true;
}

bool BigEndian16MHzInputReader::readBigEndianToBuffer(ifstream &input, float *out, size_t n) {
    auto *input_buffer = (uint16_t *)malloc(480 * 1125 * 2 * sizeof(float));
    input.read(reinterpret_cast<char *>(input_buffer), n * sizeof(uint16_t));
    for (int i = 0; i < n; i++) {
        out[i] = (float)ntohs(input_buffer[i]) / MUSE_SHORT_INPUT_MULT;
    }
    free(input_buffer);
    return input.good();
}

bool BigEndian16MHzInputReader::readShorts(ifstream &input, uint16_t *out, size_t n) {
    auto *input_buffer = (uint16_t *)malloc(480 * 1125 * 2 * sizeof(float));
    input.read(reinterpret_cast<char *>(input_buffer), n * sizeof(uint16_t));
    for (int i = 0; i < n; i++) {
        out[i] = ntohs(input_buffer[i]);
    }
    free(input_buffer);
    return input.good();
}

pair<int, pair<float, float>> BigEndian16MHzInputReader::compute_initial_skip() {
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
