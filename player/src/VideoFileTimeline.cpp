// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include "VideoFileTimeline.h"

VideoFileTimeline::VideoFileTimeline(int fps_num, int fps_den) :
        m_fps((double)fps_num / fps_den),
        m_timecode_fps((fps_num + fps_den / 2) / fps_den),
        m_steady_frames(std::max<int64_t>(2, (int64_t)std::ceil(c_steady_seconds * m_fps))),
        m_give_up_frames((int64_t)std::ceil(c_give_up_seconds * m_fps)) {
}

void VideoFileTimeline::addFrame(std::optional<double> disc_seconds, std::optional<int> chapter) {
    const int64_t frame = m_frame_count++;

    if (!m_start_decided) {
        if (disc_seconds) {
            // Where the disc's time zero falls in the file, as this frame has
            // it.  A file with a frame per field reads the same code twice, so
            // the offsets of a steady run differ by one frame, and the larger
            // one is the code's own frame.
            const int64_t offset = std::llround(*disc_seconds * m_fps) - frame;
            if (m_run_length > 0 && std::llabs(offset - m_run_first_offset) <= 1) {
                m_run_length++;
                m_run_max_offset = std::max(m_run_max_offset, offset);
            } else {
                m_run_length = 1;
                m_run_first_offset = m_run_max_offset = offset;
            }
            if (m_run_length >= m_steady_frames) {
                m_start_frames = m_run_max_offset;
                m_start_decided = true;
            }
        } else {
            m_run_length = 0;
        }
        if (!m_start_decided && m_frame_count >= m_give_up_frames)
            m_start_decided = true;
    }

    // Chapter marks come from the programme only: the frames before it (a
    // MUSE disc's pre-roll) carry a chapter number but no time
    if (disc_seconds && chapter) {
        if (m_current_chapter && *chapter == *m_current_chapter) {
            m_candidate_chapter.reset();
        } else {
            // A run ends with the number, and where the player jumped, so
            // that what it read before is from elsewhere: the disc's time
            // goes back, or the frames in between had no time at all (a
            // few without one are code lines that failed to read)
            if (!m_candidate_chapter || *m_candidate_chapter != *chapter
                    || *disc_seconds < m_candidate_last_seconds
                    || frame - m_candidate_last_frame > m_steady_frames / 3) {
                m_candidate_chapter = chapter;
                m_candidate_frame = frame;
                m_candidate_count = 0;
            }
            m_candidate_last_seconds = *disc_seconds;
            m_candidate_last_frame = frame;
            if (++m_candidate_count >= m_steady_frames) {
                // The first frames of a file have no code yet: a chapter
                // read from within the first half second is the one the
                // file starts in
                const bool at_start = m_chapters.empty() && m_candidate_frame < m_steady_frames;
                m_chapters.push_back({*chapter, at_start ? 0 : m_candidate_frame});
                m_current_chapter = chapter;
                m_candidate_chapter.reset();
            }
        }
    }
}

void VideoFileTimeline::newInput() {
    m_current_chapter.reset();
    m_candidate_chapter.reset();
    m_run_length = 0;
}

std::optional<std::string> VideoFileTimeline::startTimecode() const {
    if (!m_start_frames)
        return std::nullopt;
    const int64_t day = 24LL * 3600 * m_timecode_fps;
    const int64_t n = ((*m_start_frames % day) + day) % day;
    const int64_t seconds = n / m_timecode_fps;
    return std::format("{:02}:{:02}:{:02}:{:02}", seconds / 3600, seconds / 60 % 60, seconds % 60,
                       n % m_timecode_fps);
}
