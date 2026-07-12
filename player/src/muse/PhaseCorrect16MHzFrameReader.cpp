// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#include <map>
#include <cassert>
#include <filesystem>
#include <format>
#include "musevk/VulkanBuffer.h"
#include "MuseConstants.h"
#include "PhaseCorrect16MHzFrameReader.h"
#include "input/InputReaderFactory.h"
#include "logging/Logger.h"

using namespace std;

static constexpr off_t c_samples_per_frame = MUSE_TOTAL_HEIGHT * MUSE_TOTAL_WIDTH;

PhaseCorrect16MHzFrameReader::PhaseCorrect16MHzFrameReader(
        Logger &log,
        const std::string &filename, InputFormat input_format, double initial_seek_seconds,
        const std::optional<std::string> &output_filename)
        : FrameReader(log, filename,
                      filesystem::is_fifo(filename),
                      initial_seek_seconds, output_filename),
          m_input_format(input_format),
          m_input_reader(nullptr) {
}

bool PhaseCorrect16MHzFrameReader::initialize(std::vector<std::unique_ptr<MuseInputBlock>> &buffers) {
    auto [samples_to_skip, input_scale] = compute_initial_skip(m_log);
    m_input_scale = input_scale;

    m_input_reader = makeInputReader(m_filename, m_input_format, c_samples_per_frame);
    m_input_reader->initialize();

    m_log.info(eInput, std::format("Skipping {} initial samples", samples_to_skip));
    m_input_reader->seek(samples_to_skip);

    return FrameReader::initialize(buffers);
}

void PhaseCorrect16MHzFrameReader::cleanup() {
    FrameReader::cleanup();
    m_input_reader.reset();
}

void PhaseCorrect16MHzFrameReader::seek(double seconds) {
    if (!m_input_is_realtime) {
        std::unique_lock<std::mutex> lock(m_mutex);

        off_t frames_to_seek = (off_t)(seconds * 30);
        off_t samples_to_seek = frames_to_seek * c_samples_per_frame;
        double actual_seek_time = (double)frames_to_seek / 30.0;
        m_log.info(eInput, std::format("Seeking relative time {} s, {} samples.",
                                       actual_seek_time, samples_to_seek));
        m_input_reader->seek(samples_to_seek);

        // discard content in existing input buffers
        move(m_filled_input_buffers.begin(), m_filled_input_buffers.end(), back_inserter(m_vacant_input_buffers));
        m_filled_input_buffers.clear();
        m_cv_vacant.notify_one();
    }
}

void PhaseCorrect16MHzFrameReader::threadFunc() {
    for (;;) {
        unique_ptr<MuseInputBlock> buffer = nullptr;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv_vacant.wait(lock, [this]{return m_stop_request || !m_vacant_input_buffers.empty();});
            if (m_stop_request)
                break;
            buffer = std::move(m_vacant_input_buffers.front());
            m_vacant_input_buffers.pop_front();
        }

        buffer->input_samples_per_muse_sample = 1;
        auto data = buffer->video_data->data<float>();
        if (m_input_reader->readFloats(data) != (int)c_samples_per_frame)
            break;
        for (off_t i = 0; i < c_samples_per_frame; i++)
            data[i] = data[i] * m_input_scale.first + m_input_scale.second;
        memset(buffer->dropout_data->data<uint8_t>(), 0, sizeof(buffer->dropout_data->size()));

        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv_filled.notify_one();
        m_filled_input_buffers.push_back(std::move(buffer));
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv_filled.notify_one();
    m_reader_thread_finished = true;
}

pair<int, pair<float, float>> PhaseCorrect16MHzFrameReader::compute_initial_skip(Logger &log) {
    constexpr off_t two_frame_size = 480 * 1125 * 2;
    auto reader = makeInputReader(m_filename, m_input_format, two_frame_size);
    reader->initialize();

    vector<float> buffer(two_frame_size);
    if (reader->readFloats(buffer.data()) != (int)two_frame_size)
        throw runtime_error("PhaseCorrect16MHzFrameReader: not enough input data to compute initial skip");

    auto sorted(buffer);
    sort(sorted.begin(), sorted.end());
    auto y1 = sorted[500];
    auto y2 = sorted[480 * 1125 * 2 - 500];
    float a = (239.0f - 16.0f) / (y2 - y1);
    pair<float, float> input_scale = {a, 16.0f - y1 * a};
    log.info(eInput, std::format("Computed input scale: {}, {}", input_scale.first, input_scale.second));

    // The rest of the code only checks the signal for rising or falling values, and never uses the actual
    // values directly.  We do not need to do any rescaling for this.

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

    return { bestLineOffset * 480 + bestPixelOffset, input_scale };
}
