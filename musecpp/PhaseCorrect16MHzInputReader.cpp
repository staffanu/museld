//
// Created by staffanu on 6/30/23.
//

#include <netinet/in.h>
#include <map>
#include <cassert>
#include <filesystem>
#include <fmt/format.h>
#include "musevk/VulkanBuffer.h"
#include "MuseTypes.h"
#include "PhaseCorrect16MHzInputReader.h"
#include "util/Logger.h"

using namespace std;

PhaseCorrect16MHzInputReader::PhaseCorrect16MHzInputReader(
        Logger &log,
        const std::string &filename, bool big_endian, double initial_seek_seconds,
        const std::optional<std::string> &output_filename)
        : InputReader(log, filename,
                      filesystem::is_fifo(filename),
                      initial_seek_seconds, output_filename),
        m_input{},
        m_big_endian(big_endian) {
}

bool PhaseCorrect16MHzInputReader::initialize(std::vector<std::unique_ptr<InputReader::InputReaderBlock>> &buffers) {
    auto [samples_to_skip, eq] = compute_initial_skip(m_log);

    m_input = ifstream(static_cast<string>(m_filename).c_str(), ios::binary | ios::in);
    m_input.exceptions(ifstream::badbit);

    off_t samples_per_frame = MUSE_TOTAL_HEIGHT * MUSE_TOTAL_WIDTH;
    assert(samples_per_frame >= samples_to_skip);
    vector<float> skip_buffer(samples_per_frame);
    m_log.info(eInput, fmt::format("Skipping {} initial samples", samples_to_skip));
    readFloats(m_input, skip_buffer.data(), samples_to_skip);

    return InputReader::initialize(buffers);
}

void PhaseCorrect16MHzInputReader::cleanup() {
    InputReader::cleanup();
    m_input.close();
}

void PhaseCorrect16MHzInputReader::seek(double seconds) {
    if (!m_input_is_realtime) {
        std::unique_lock<std::mutex> lock(m_mutex);

        off_t samples_per_frame = MUSE_TOTAL_HEIGHT * MUSE_TOTAL_WIDTH;
        off_t frames_to_seek = (off_t)(seconds * 30);

        // ensure that we do not seek to before the beginning of the file
        auto current_pos = m_input.tellg();
        if (current_pos < -2 * frames_to_seek * samples_per_frame)
            frames_to_seek = -current_pos / 2 / samples_per_frame;

        off_t samples_to_seek = frames_to_seek * samples_per_frame;
        double actual_seek_time = (double)frames_to_seek / 30.0;
        m_log.info(eInput, fmt::format("Seeking relative time {} s, {} samples.",
                                       actual_seek_time, samples_to_seek));
        m_input.seekg(samples_to_seek * 2, ifstream::cur);

        // discard content in existing input buffers
        move(m_filled_muse_input_buffers.begin(), m_filled_muse_input_buffers.end(), back_inserter(m_vacant_muse_input_buffers));
        m_filled_muse_input_buffers.clear();
        m_log.error(eInput, fmt::format("sizes: {} {}", m_vacant_muse_input_buffers.size(), m_filled_muse_input_buffers.size()));
        m_cv_vacant.notify_one();
    }
}

void PhaseCorrect16MHzInputReader::threadFunc() {
    for (;;) {
        unique_ptr<InputReaderBlock> buffer = nullptr;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv_vacant.wait(lock, [this]{return m_stop_request || !m_vacant_muse_input_buffers.empty();});
            if (m_stop_request)
                break;
            buffer = std::move(m_vacant_muse_input_buffers.front());
            m_vacant_muse_input_buffers.pop_front();
        }

        auto data = buffer->video_data->data<float>();
        if (!readFloats(m_input, data, MUSE_TOTAL_HEIGHT * MUSE_TOTAL_WIDTH))
            break;
        memset(buffer->dropout_data->data<uint8_t>(), 0, sizeof(buffer->dropout_data->size()));

        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv_filled.notify_one();
        m_filled_muse_input_buffers.push_back(std::move(buffer));
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv_filled.notify_one();
    m_reader_thread_finished = true;
}

bool PhaseCorrect16MHzInputReader::readFloats(ifstream &input, float *out, size_t n) {
    auto *input_buffer = (uint16_t *)malloc(n * sizeof(uint16_t));
    input.read(reinterpret_cast<char *>(input_buffer), n * sizeof(uint16_t));
    for (int i = 0; i < n; i++) {
        out[i] = (float)(m_big_endian ? ntohs(input_buffer[i]) : input_buffer[i]) / 4.0f;
    }
    free(input_buffer);
    return input.good();
}

pair<int, pair<float, float>> PhaseCorrect16MHzInputReader::compute_initial_skip(Logger &log) {
    ifstream input(static_cast<string>(m_filename).c_str(), ios::binary | ios::in);
    input.exceptions(ifstream::badbit);
    vector<float> buffer(480 * 1125 * 2); // two frames of data
    readFloats(input, buffer.data(), 480 * 1125 * 2);

    auto sorted(buffer);
    sort(sorted.begin(), sorted.end());
    auto y1 = sorted[500];
    auto y2 = sorted[480 * 1125 * 2 - 500];
    pair<float, float> eq = {(y2 - y1) / (239.0f - 16.0f), y1 - 16.0f};
    log.info(eInput, fmt::format("Initial eq: {}, {}", eq.first, eq.second));

    // The rest of the code only checks the signal for rising or falling values, and never uses the actual
    // values directly.  We do not need to do any equalization for this.

    // checks if the m_line starting at off has a positive or a negative sync
    auto isSyncGood = [&buffer](int off, bool expectedPositive) {
        bool isPos = buffer[off + 3] < buffer[off + 5] && buffer[off + 5] < buffer[off + 7];
        bool isNeg = buffer[off + 3] > buffer[off + 5] && buffer[off + 5] > buffer[off + 7];
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
