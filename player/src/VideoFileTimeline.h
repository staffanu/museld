// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_VIDEOFILETIMELINE_H
#define MUSECPP_VIDEOFILETIMELINE_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// What a --write file says about where on the disc it is: the disc time of
// its first frame, for the container's start timecode, and the frames its
// chapters start at.  Fed the disc code of every frame written, in order.
//
// This is the pure decision logic, with no I/O (the file writer acts on it),
// so it is testable against a synthetic disc.
//
// The start is not simply the first code read.  A capture of a side from its
// beginning starts in the lead-in or the pre-roll, with no time code, and a
// player still spinning up may jump back a few tracks before it settles, so
// the first codes do not run on into the programme.  The start is therefore
// taken from the first half second of frames whose codes advance with the
// file, extrapolated back to frame 0 -- which gives a time before zero for a
// file that begins ahead of the programme.  The container's header carries
// the timecode, so the writer holds the header back until startDecided().
class VideoFileTimeline {
public:
    struct Chapter {
        int number;      // as the disc numbers it
        int64_t frame;   // the file frame it starts at
    };

    VideoFileTimeline(int fps_num, int fps_den);

    // One call per frame written.  disc_seconds: the disc's playback time of
    // the frame (DiscInfo::playbackTimeSeconds), chapter: its chapter number;
    // unset when the frame's code does not carry them.
    void addFrame(std::optional<double> disc_seconds, std::optional<int> chapter);

    // The next frame comes from another input written into the same file
    // (the next side of the disc): its first chapter gets a mark even when
    // its number is the one the last input ended in
    void newInput();

    // True once the start is known or has been given up on (no steady run of
    // codes in the first c_give_up_seconds).  giveUp() settles it at once.
    [[nodiscard]] bool startDecided() const { return m_start_decided; }
    void giveUp() { m_start_decided = true; }

    // The disc time of file frame 0, in file frames; negative when the file
    // starts before the disc's time zero.  Unset while undecided or given up.
    [[nodiscard]] std::optional<int64_t> startFrames() const { return m_start_frames; }

    // The start as an SMPTE-style non-drop timecode HH:MM:SS:FF counting
    // timecodeFps() frames per second.  Timecode has no sign: a start before
    // zero wraps below 24 hours (23:59:58:25 for 35 frames before zero at 30
    // frames/s), the tape convention for pre-roll, and the timecode then
    // passes 00:00:00:00 where the disc's time does.
    [[nodiscard]] std::optional<std::string> startTimecode() const;
    [[nodiscard]] int timecodeFps() const { return m_timecode_fps; }

    [[nodiscard]] const std::vector<Chapter> &chapters() const { return m_chapters; }
    [[nodiscard]] int64_t frameCount() const { return m_frame_count; }

    static constexpr double c_steady_seconds = 0.5;    // of agreeing codes, for the start and for a chapter
    static constexpr double c_give_up_seconds = 60.0;  // without a steady run: no timecode

private:
    double m_fps;
    int m_timecode_fps;      // the frame rate rounded: 30 for 30000/1001
    int64_t m_steady_frames;
    int64_t m_give_up_frames;
    int64_t m_frame_count = 0;

    // The run of frames whose disc time minus file position agrees
    int64_t m_run_length = 0;
    int64_t m_run_first_offset = 0;
    int64_t m_run_max_offset = 0;
    bool m_start_decided = false;
    std::optional<int64_t> m_start_frames;

    // A chapter number counts once it has been read for c_steady_seconds of
    // frames with no jump among them: one misread code line must not leave
    // two marks, nor the frames a player reads before it jumps back
    std::vector<Chapter> m_chapters;
    std::optional<int> m_current_chapter;
    std::optional<int> m_candidate_chapter;
    int64_t m_candidate_frame = 0;
    int64_t m_candidate_count = 0;
    double m_candidate_last_seconds = 0;
    int64_t m_candidate_last_frame = 0;
};

#endif //MUSECPP_VIDEOFILETIMELINE_H
