// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_CHAPTERSEARCH_H
#define MUSECPP_CHAPTERSEARCH_H

#include <cstdint>
#include <optional>
#include <string>

// Finds where a chapter starts in a capture by probing it: seek somewhere,
// read the disc code of the first frame decoded there, decide where to look
// next.  Laserdiscs carry no index of their chapters in the programme area
// (and the captures rarely include the lead-in that could hold one), so the
// chapter boundary has to be searched for, but the search is cheap -- a
// probe costs one seek plus one frame -- and short: the capture is a
// realtime recording, so disc time is proportional to the input offset, and
// chapters are counted, so a guess interpolated between the chapters seen
// so far is close.
//
// This class is the pure decision logic, with no I/O or threads: the player
// performs the seeks it asks for and feeds back what the decoder read, which
// keeps the search testable against a synthetic disc.  Everything is in
// input sample offsets.
class ChapterSearch {
public:
    struct Params {
        double samples_per_second;      // the input's sample rate
        int64_t input_sample_count;     // the input's length, -1 when unknown
        // The search ends when the chapter start is pinned down to this
        // span, and playback resumes at the end of the span (the side
        // known to be inside the chapter)
        double tolerance_seconds = 0.15;
        // "Previous chapter" from within this many seconds of the current
        // chapter's start goes to the chapter before it, as the players do
        double restart_slack_seconds = 3.0;
        // Probes jump this far at first when the bracket is open on one
        // side, doubling until the target chapter is passed
        double initial_step_seconds = 30.0;
        // The first frames after a seek may come without a chapter number
        // (CLV discs carry it on alternate fields and the flag is inherited
        // frame to frame; a dropout can take a line): wait this many frames
        // before moving the probe a little and trying again
        int frames_without_chapter_limit = 4;
    };

    // What the decoder read at one position
    struct Reading {
        int64_t offset;                 // input sample offset of the frame
        std::optional<int> chapter;
        bool lead_in = false;
        bool lead_out = false;
    };

    enum class Direction { eNext, ePrevious };

    struct Action {
        enum class Kind {
            eSeek,      // probe at `offset`
            eWait,      // keep feeding frames from the current probe
            eDone,      // chapter `chapter` starts at `offset`
            eFailed,    // give up; `reason` says why (upper case, for the OSD)
        };
        Kind kind;
        int64_t offset = 0;
        int chapter = 0;
        std::string reason;
    };

    // `current` is where playback is now; its chapter must be known
    ChapterSearch(const Params &params, const Reading &current, Direction direction);
    // The start of a given chapter (the --chapter option); the current
    // chapter decides which way to look
    ChapterSearch(const Params &params, const Reading &current, int target_chapter);

    // The action the caller should carry out now.  After an eSeek the caller
    // feeds the frames decoded at the new position to feed(); eDone and
    // eFailed end the search.
    [[nodiscard]] const Action &action() const { return m_action; }

    // A frame decoded after the pending seek (and not before it: the caller
    // tells them apart by the reader's seek generation).  Returns the next
    // action, also available from action().
    const Action &feed(const Reading &reading);

    [[nodiscard]] int targetChapter() const { return m_target; }
    [[nodiscard]] int probes() const { return m_probes; }
    [[nodiscard]] int64_t originOffset() const { return m_origin.offset; }

private:
    struct Bound {
        int64_t offset;
        // The chapter number read there; unset for lead-in/lead-out and the
        // input ends, where only the side of the target is known
        std::optional<int> chapter;
    };

    void probe(int64_t offset);
    void decide();
    void finish();
    [[nodiscard]] int64_t seconds(double s) const { return (int64_t)(s * m_params.samples_per_second); }
    [[nodiscard]] int64_t clampToInput(int64_t offset) const;

    const Params m_params;
    const Direction m_direction;
    const Reading m_origin;
    int m_target;
    std::optional<Bound> m_lo;          // chapter < target (or lead-in)
    std::optional<Bound> m_hi;          // chapter >= target (or lead-out, or the end)
    int64_t m_step;                     // galloping step while a bound is unknown
    int64_t m_requested;                // the pending probe's position
    int64_t m_landing_bias;             // how far past the request the frames land
    int m_frames_without_chapter;
    int m_retries_without_chapter;
    int m_probes;
    int m_wasted;                       // consecutive probes that moved no bound
    bool m_chained;                     // ePrevious already moved on to the chapter before
    Action m_action;
};

#endif //MUSECPP_CHAPTERSEARCH_H
