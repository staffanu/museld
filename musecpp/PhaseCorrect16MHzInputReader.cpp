//
// Created by staffanu on 6/30/23.
//

#include <netinet/in.h>
#include <map>
#include <fmt/format.h>
#include "MuseTypes.h"
#include "PhaseCorrect16MHzInputReader.h"

using namespace std;

PhaseCorrect16MHzInputReader::PhaseCorrect16MHzInputReader(
        Logger &log,
        const std::string &filename, bool big_endian, bool input_is_fifo, double initial_seek_seconds,
        const std::optional<std::string> &output_filename)
        : InputReader(log, filename, input_is_fifo, initial_seek_seconds, output_filename),
        m_input{},
        m_big_endian(big_endian) {
}

bool PhaseCorrect16MHzInputReader::initialize(std::vector<std::shared_ptr<musevk::VulkanBuffer>> const &buffers) {
    auto [samples_to_skip, eq] = compute_initial_skip(m_log);

    m_input = ifstream(static_cast<string>(m_filename).c_str(), ios::binary | ios::in);
    m_input.exceptions(ifstream::badbit);

    off_t samples_per_frame = MUSE_TOTAL_HEIGHT * MUSE_TOTAL_WIDTH;
    assert(samples_per_frame >= samples_to_skip);
    vector<uint16_t> skip_buffer(samples_per_frame);
    m_log.info(eInput, fmt::format("Skipping {} initial samples", samples_to_skip));
    readShorts(m_input, skip_buffer.data(), samples_to_skip);

    return InputReader::initialize(buffers);
}

void PhaseCorrect16MHzInputReader::cleanup() {
    InputReader::cleanup();
    m_input.close();
}

void PhaseCorrect16MHzInputReader::seek(double seconds) {
    if (!m_input_is_fifo) {
        // FIXME: make this thread safe!
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
    }
}

void PhaseCorrect16MHzInputReader::threadFunc() {
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

bool PhaseCorrect16MHzInputReader::readShorts(ifstream &input, uint16_t *out, size_t n) {
    auto *input_buffer = (uint16_t *)malloc(480 * 1125 * 2 * sizeof(float));
    input.read(reinterpret_cast<char *>(input_buffer), n * sizeof(uint16_t));
    for (int i = 0; i < n; i++) {
        out[i] = m_big_endian ? ntohs(input_buffer[i]) : input_buffer[i];
    }
    free(input_buffer);
    return input.good();
}

pair<int, pair<float, float>> PhaseCorrect16MHzInputReader::compute_initial_skip(Logger &log) {
    ifstream input(static_cast<string>(m_filename).c_str(), ios::binary | ios::in);
    input.exceptions(ifstream::badbit);
    vector<uint16_t> buffer(480 * 1125 * 2); // two frames of data
    readShorts(input, buffer.data(), 480 * 1125 * 2);

    auto sorted(buffer);
    sort(sorted.begin(), sorted.end());
    auto y1 = sorted[500];
    auto y2 = sorted[480 * 1125 * 2 - 500];
    pair<float, float> eq = {(y2 - y1) / (MUSE_SHORT_INPUT_MULT * (239.0f - 16.0f)), (float)y1 / MUSE_SHORT_INPUT_MULT - 16.0f};
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
