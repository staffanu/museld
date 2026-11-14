// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_POSITIONSEARCH_H
#define MUSECPP_POSITIONSEARCH_H

#include <cstdint>
#include <optional>
#include <string>

// Finds a position on the disc in a capture by probing it: seek somewhere,
// read the disc code of the first frame decoded there, decide where to look
// next.  The position is given as the disc counts it -- a chapter, a time
// of the CLV time code, or a CAV frame number -- which is what a player's
// remote offers.  Laserdiscs carry no index of their chapters in the
// programme area (and the captures rarely include the lead-in that could
// hold one), so a chapter boundary has to be searched for, but the search
// is cheap -- a probe costs one seek plus one frame -- and short: the
// capture is a realtime recording, so disc time is proportional to the
// input offset, and chapters are counted, so a guess interpolated between
// the chapters seen so far is close.  A time or a frame number is a point
// on that line, so the first probe, interpolated from where playback is,
// lands within a second of it, and a second or third pins it down.
//
// This class is the pure decision logic, with no I/O or threads: the player
// performs the seeks it asks for and feeds back what the decoder read, which
// keeps the search testable against a synthetic disc.  Everything is in
// input sample offsets.
class PositionSearch {
public:
    struct Params {
        double samples_per_second;      // the input's sample rate
        int64_t input_sample_count;     // the input's length, -1 when unknown
        // The input's frame rate: what a frame number counts, and a second
        // of the time code is round(frames_per_second) frames of it
        double frames_per_second = 30000.0 / 1001.0;
        // A chapter or time search ends when the position is pinned down to
        // this span, and playback resumes at the end of the span (the side
        // known to be at or past the target); a frame search goes to the
        // frame
        double tolerance_seconds = 0.15;
        // "Previous chapter" from within this many seconds of the current
        // chapter's start goes to the chapter before it, as the players do
        double restart_slack_seconds = 3.0;
        // Probes jump this far at first when the bracket is open on one
        // side, doubling until the target chapter is passed; a time or
        // frame probe starts a second beyond its estimate instead
        double initial_step_seconds = 30.0;
        // The first frames after a seek may come without the code (CLV discs
        // carry the chapter on alternate fields and the flag is inherited
        // frame to frame; a dropout can take a line): wait this many frames
        // before moving the probe a little and trying again
        int frames_without_code_limit = 4;
    };

    // What the decoder read at one position
    struct Reading {
        int64_t offset;                 // input sample offset of the frame
        std::optional<int> chapter;
        // The disc's time code in its own seconds (DiscInfo::discTimeSeconds),
        // and its step: a frame, or a whole second on a CLV disc whose code
        // has no picture numbers
        std::optional<double> time;
        double time_resolution = 1.0;
        std::optional<int> frame;       // the CAV picture number
        bool lead_in = false;
        bool lead_out = false;
    };

    // Where to go
    struct Target {
        enum class Kind {
            eNextChapter,   // the chapter after the current one
            ePreviousChapter, // the start of the current chapter, or the one before from just inside it
            eChapter,       // the start of chapter `value`
            eTime,          // the time code `value`, in the disc's seconds
            eFrame,         // the CAV frame `value`
        };
        Kind kind;
        double value = 0;
        static Target nextChapter() { return {Kind::eNextChapter}; }
        static Target previousChapter() { return {Kind::ePreviousChapter}; }
        static Target chapter(int chapter) { return {Kind::eChapter, (double)chapter}; }
        static Target time(double seconds) { return {Kind::eTime, seconds}; }
        static Target frame(int frame) { return {Kind::eFrame, (double)frame}; }
        [[nodiscard]] bool isChapter() const { return kind != Kind::eTime && kind != Kind::eFrame; }
    };

    struct Action {
        enum class Kind {
            eSeek,      // probe at `offset`
            eWait,      // keep feeding frames from the current probe
            eDone,      // the position is at `offset`; `value` is the chapter found, or the time or frame asked for
            eFailed,    // give up; `reason` says why (upper case, for the OSD)
        };
        Kind kind;
        int64_t offset = 0;
        double value = 0;
        std::string reason;
    };

    // `current` is where playback is now; it must carry the coordinate the
    // target is given in (the chapter, time or frame), else the search fails
    // at once with the reason on action()
    PositionSearch(const Params &params, const Reading &current, const Target &target);

    // The action the caller should carry out now.  After an eSeek the caller
    // feeds the frames decoded at the new position to feed(); eDone and
    // eFailed end the search.
    [[nodiscard]] const Action &action() const { return m_action; }

    // A frame decoded after the pending seek (and not before it: the caller
    // tells them apart by the reader's seek generation).  Returns the next
    // action, also available from action().
    const Action &feed(const Reading &reading);

    // The pending probe ran into the end of the input without a frame: the
    // capture's tail has no signal (the player had stopped, or the capture
    // ran on past the disc).  A forward search then looks a little earlier,
    // further back each time, for the last frames there are; what they read
    // decides as a reading at the input's end would.  Returns the next action.
    const Action &probeReachedEnd();

    [[nodiscard]] const Target &target() const { return m_target; }
    [[nodiscard]] int probes() const { return m_probes; }
    [[nodiscard]] int64_t originOffset() const { return m_origin.offset; }

    // The position as the OSD names it: "CHAPTER 12", "0:51:31", "FRAME 23456"
    static std::string describe(Target::Kind kind, double value);
    // The reading's coordinate for the target's kind
    static std::optional<double> coordinate(const Reading &reading, Target::Kind kind);

private:
    struct Bound {
        int64_t offset;
        // The coordinate read there; unset for lead-in/lead-out and the
        // input ends, where only the side of the target is known
        std::optional<double> value;
    };

    void probe(int64_t offset);
    void decide();
    void finish();
    [[nodiscard]] int64_t seconds(double s) const { return (int64_t)(s * m_params.samples_per_second); }
    [[nodiscard]] int64_t clampToInput(int64_t offset) const;
    // Input samples per unit of the coordinate: a time code second (which is
    // round(fps) frames), a frame; unused for chapters
    [[nodiscard]] double samplesPerUnit() const;
    void noteResolution(const Reading &reading);
    [[nodiscard]] int64_t tolerance() const;
    [[nodiscard]] const char *noCodeReason() const;
    [[nodiscard]] const char *notOnCaptureReason() const;

    const Params m_params;
    Target m_target;
    const bool m_forward;
    const Reading m_origin;
    std::optional<Bound> m_lo;          // coordinate < target (or lead-in)
    std::optional<Bound> m_hi;          // coordinate >= target (or lead-out, or the end)
    int64_t m_step;                     // galloping step while a bound is unknown
    int64_t m_requested;                // the pending probe's position
    int64_t m_landing_bias;             // how far past the request the frames land
    int m_frames_without_code;
    int m_retries_without_code;
    int m_probes;
    int m_wasted;                       // consecutive probes that moved no bound
    bool m_chained;                     // ePreviousChapter already moved on to the chapter before
    bool m_coarse;                      // the time code counts whole seconds, not frames
    bool m_past_end;                    // a probe found nothing up to the input's end
    int64_t m_back_step;                // how far before it the next probe looks
    Action m_action;
};

#endif //MUSECPP_POSITIONSEARCH_H
