// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "PositionSearch.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace {

using Kind = PositionSearch::Target::Kind;

// Which way the search goes from the current position
bool forwardOf(const PositionSearch::Reading &current, const PositionSearch::Target &target) {
    switch (target.kind) {
        case Kind::eNextChapter: return true;
        case Kind::ePreviousChapter: return false;
        case Kind::eChapter:
        case Kind::eTime:
        case Kind::eFrame: {
            const auto here = PositionSearch::coordinate(current, target.kind);
            return here && *here < target.value;
        }
    }
    return true;
}

} // namespace

std::optional<double> PositionSearch::coordinate(const Reading &reading, Target::Kind kind) {
    switch (kind) {
        case Kind::eNextChapter:
        case Kind::ePreviousChapter:
        case Kind::eChapter:
            return reading.chapter ? std::make_optional((double)*reading.chapter) : std::nullopt;
        case Kind::eTime:
            return reading.time;
        case Kind::eFrame:
            return reading.frame ? std::make_optional((double)*reading.frame) : std::nullopt;
    }
    return std::nullopt;
}

std::string PositionSearch::describe(Target::Kind kind, double value) {
    switch (kind) {
        case Kind::eTime: {
            const int s = (int)std::lround(value);
            return std::format("{}:{:02}:{:02}", s / 3600, s % 3600 / 60, s % 60);
        }
        case Kind::eFrame:
            return std::format("FRAME {}", (int)std::lround(value));
        default:
            return std::format("CHAPTER {}", (int)std::lround(value));
    }
}

const char *PositionSearch::noCodeReason() const {
    switch (m_target.kind) {
        case Kind::eTime: return "NO TIME CODE";
        case Kind::eFrame: return "NO FRAME NUMBER";
        default: return "NO CHAPTER CODE";
    }
}

const char *PositionSearch::notOnCaptureReason() const {
    return m_target.isChapter() ? "NO MORE CHAPTERS" : "NOT FOUND";
}

void PositionSearch::noteResolution(const Reading &reading) {
    if (m_target.kind == Kind::eTime && reading.time && reading.time_resolution > 1.5 / std::round(m_params.frames_per_second))
        m_coarse = true;
}

double PositionSearch::samplesPerUnit() const {
    switch (m_target.kind) {
        case Kind::eTime:
            // A second of the time code is a whole number of frames (30 on
            // NTSC), which play in round(fps)/fps seconds
            return m_params.samples_per_second * std::round(m_params.frames_per_second) / m_params.frames_per_second;
        case Kind::eFrame:
            return m_params.samples_per_second / m_params.frames_per_second;
        default:
            return 0;
    }
}

int64_t PositionSearch::tolerance() const {
    // A frame search goes to the frame: a bracket of adjacent frames
    if (m_target.kind == Kind::eFrame)
        return seconds(1.5 / m_params.frames_per_second);
    return seconds(m_params.tolerance_seconds);
}

PositionSearch::PositionSearch(const Params &params, const Reading &current, const Target &target)
        : m_params(params),
          m_target(target),
          m_forward(forwardOf(current, target)),
          m_origin(current),
          m_step(seconds(target.isChapter() ? params.initial_step_seconds : 1.0)),
          m_requested(current.offset),
          // Frames land a little after the requested position: the input
          // reader's block granularity and the first whole frame after the
          // seek.  Two frames to start with; measured from then on.
          m_landing_bias(seconds(2.0 / 30.0)),
          m_frames_without_code(0),
          m_retries_without_code(0),
          m_probes(0),
          m_wasted(0),
          // Only "previous chapter" moves on to the chapter before; an
          // explicit target never does
          m_chained(target.kind != Kind::ePreviousChapter),
          m_coarse(false),
          m_past_end(false),
          m_back_step(seconds(5.0)),
          m_action{Action::Kind::eFailed} {
    const auto here = coordinate(current, target.kind);
    if (!here) {
        m_action = {Action::Kind::eFailed, 0, 0, noCodeReason()};
        return;
    }
    noteResolution(current);
    if (params.input_sample_count < 0 && m_forward) {
        // Probing past the end of the input finishes the decoding threads
        // for good, so forward probes need to know where the end is
        m_action = {Action::Kind::eFailed, 0, 0, "INPUT LENGTH UNKNOWN"};
        return;
    }
    switch (target.kind) {
        case Kind::eNextChapter:
            m_target.value = *here + 1;
            m_lo = Bound{current.offset, here};
            break;
        case Kind::ePreviousChapter:
            // The start of the current chapter first; finish() moves on to
            // the one before when that turns out to be right behind us
            m_target.value = *here;
            m_hi = Bound{current.offset, here};
            break;
        case Kind::eChapter:
        case Kind::eTime:
        case Kind::eFrame:
            if (target.kind == Kind::eFrame && *here == target.value) {
                // Already on it
                m_action = {Action::Kind::eDone, current.offset, target.value};
                return;
            }
            // Inside the target chapter, or at or past the time or frame:
            // look for its start behind us
            if (*here >= target.value)
                m_hi = Bound{current.offset, here};
            else
                m_lo = Bound{current.offset, here};
            break;
    }
    decide();
}

int64_t PositionSearch::clampToInput(int64_t offset) const {
    offset = std::max<int64_t>(0, offset);
    if (m_params.input_sample_count >= 0) {
        // A second short of the end: the decoder needs a whole frame there
        offset = std::min(offset, std::max<int64_t>(0, m_params.input_sample_count - seconds(1.0)));
    }
    return offset;
}

void PositionSearch::probe(int64_t offset) {
    m_requested = clampToInput(offset);
    m_frames_without_code = 0;
    m_probes++;
    m_action = {Action::Kind::eSeek, m_requested};
}

void PositionSearch::finish() {
    // The target is known to have been reached by m_hi
    const int64_t start = m_hi->offset;
    if (!m_chained && m_origin.offset - start < seconds(m_params.restart_slack_seconds) && m_target.value > 0) {
        // Already at the start of this chapter: the previous one is meant.
        // The bracket is known on one side: this chapter's start is at or
        // past the previous chapter's end.
        m_chained = true;
        m_target.value--;
        m_hi = Bound{start, m_target.value + 1};
        m_lo.reset();
        m_step = seconds(m_params.initial_step_seconds);
        decide();
        return;
    }
    const double value = m_target.isChapter() ? m_hi->value.value_or(m_target.value) : m_target.value;
    m_action = {Action::Kind::eDone, start, value};
}

void PositionSearch::decide() {
    if (m_hi && m_target.kind == Kind::eFrame && m_hi->value && *m_hi->value == m_target.value) {
        // The frame itself was read: nothing before it can be the target
        finish();
        return;
    }

    if (m_lo && m_hi) {
        if (m_hi->offset - m_lo->offset <= tolerance()) {
            finish();
            return;
        }
        const int64_t span = m_hi->offset - m_lo->offset;
        double fraction = 0.5;
        if (m_target.isChapter()) {
            // Between the chapters read at the two bounds lie (hi - lo)
            // chapter starts; assuming them evenly spaced puts the target's
            // start at a definite fraction of the bracket.  With adjacent
            // chapters at the bounds this is the midpoint, i.e. plain
            // bisection.  Guessing too close to a bound would waste a probe
            // on the landing slack, so the guess stays inside the middle
            // 80 % of the bracket.
            if (m_lo->value && m_hi->value && *m_hi->value > *m_lo->value)
                fraction = (m_target.value - *m_lo->value) / (*m_hi->value - *m_lo->value + 1);
            fraction = std::clamp(fraction, 0.1, 0.9);
        } else {
            // Time and frames run at the capture's rate: the target sits at
            // its fraction of the bracket to within a frame or two, however
            // wide the bracket, so the guess is not kept away from the bounds,
            // and an upper bound reading the target itself is the frame
            // looked for.  A time code that counts whole seconds places its
            // bounds within a second only: the guess from a wide bracket is
            // still good to that, but a bracket of a few seconds says nothing
            // about where inside their seconds the bounds were read, and is
            // bisected down to the tolerance.
            if (m_lo->value && m_hi->value && *m_hi->value > *m_lo->value) {
                fraction = (m_target.value - *m_lo->value) / (*m_hi->value - *m_lo->value);
                const double frame_unit = m_target.kind == Kind::eTime ? 1.0 / std::round(m_params.frames_per_second) : 1.0;
                if (!m_coarse && *m_hi->value - m_target.value < 0.5 * frame_unit) {
                    finish();
                    return;
                }
                fraction = m_coarse && span < seconds(3.0) ? 0.5 : std::clamp(fraction, 0.0, 1.0);
            }
        }
        int64_t guess = m_lo->offset + (int64_t)(fraction * (double)span);
        if (m_coarse && span >= seconds(3.0)) {
            // A bound's second began up to a second before it was read: the
            // target is at least that far inside the bracket
            const auto unit = (int64_t)samplesPerUnit();
            guess = std::clamp(guess, m_lo->offset + unit, std::max(m_lo->offset + unit, m_hi->offset - unit));
        }
        // Aim short of the guess by the measured landing slack, and never
        // at or before the lower bound (whose frame we would read again)
        probe(std::max(m_lo->offset + 1, guess - m_landing_bias));
        return;
    }

    if (m_lo) {
        // Forward: gallop from the lower bound until the target is passed.
        // The input end is a bound on its own: a probe clamped there that
        // still reads below the target means the position is not on this
        // capture, handled in feed().  A time or frame is estimated from
        // the bound and the step is the margin past it.
        int64_t from = m_lo->offset;
        if (!m_target.isChapter() && m_lo->value)
            from += (int64_t)((m_target.value - *m_lo->value) * samplesPerUnit());
        probe(from + m_step);
        m_step *= 2;
        return;
    }

    // Backward: gallop from the upper bound towards the start
    if (m_hi->offset == 0) {
        // The position was before the capture began
        m_action = {Action::Kind::eDone, 0, m_target.isChapter() ? m_hi->value.value_or(m_target.value) : m_target.value};
        return;
    }
    int64_t from = m_hi->offset;
    if (!m_target.isChapter() && m_hi->value)
        from += (int64_t)((m_target.value - *m_hi->value) * samplesPerUnit());
    probe(from - m_step);
    m_step *= 2;
}

const PositionSearch::Action &PositionSearch::feed(const Reading &reading) {
    if (m_action.kind != Action::Kind::eSeek && m_action.kind != Action::Kind::eWait)
        return m_action;

    // A frame that did not come from where the probe was aimed carries no
    // information about that position: the readers hand out the first whole
    // frame after the seek, which lands somewhat past the request, never
    // far from it
    const int64_t landed = reading.offset - m_requested;
    if (landed < 0 || landed > seconds(2.0)) {
        m_action = {Action::Kind::eWait};
        return m_action;
    }

    // Keep a running estimate of how far past the request the frames land,
    // so the guesses can be aimed short of where they should read
    m_landing_bias = (m_landing_bias * 3 + landed) / 4;

    const auto value = coordinate(reading, m_target.kind);
    noteResolution(reading);
    if (!value && !reading.lead_in && !reading.lead_out) {
        if (++m_frames_without_code < m_params.frames_without_code_limit) {
            m_action = {Action::Kind::eWait};
            return m_action;
        }
        // Nothing to read here: a stretch without codes, or a disc without
        // them at all.  Nudge the probe forward a little, a few times.
        if (++m_retries_without_code > 3) {
            m_action = {Action::Kind::eFailed, 0, 0, noCodeReason()};
            return m_action;
        }
        probe(m_requested + seconds(1.0));
        return m_action;
    }

    // Which side of the target is this frame on?
    const bool below = reading.lead_in || (value && *value < m_target.value && !reading.lead_out);
    const Bound bound{reading.offset, reading.lead_in || reading.lead_out ? std::nullopt : value};
    const auto lo_before = m_lo ? m_lo->offset : -1;
    const auto hi_before = m_hi ? m_hi->offset : -1;
    if (below) {
        if (m_hi && reading.offset >= m_hi->offset) {
            // Landed past the upper bound and still below the target: the
            // disc's codes contradict themselves here (a dropout misread,
            // or chapters out of order); accept the upper bound
            m_action = {Action::Kind::eDone, m_hi->offset,
                        m_target.isChapter() ? m_hi->value.value_or(m_target.value) : m_target.value};
            return m_action;
        }
        const bool at_input_end = m_params.input_sample_count >= 0
                && m_requested >= clampToInput(m_params.input_sample_count);
        if ((at_input_end || m_past_end) && m_forward && !m_hi) {
            m_action = {Action::Kind::eFailed, 0, 0, notOnCaptureReason()};
            return m_action;
        }
        if (!m_lo || reading.offset > m_lo->offset)
            m_lo = bound;
    } else {
        if (reading.lead_out && m_forward && !m_hi) {
            // Beyond the programme: the position does not exist.  (A
            // lead-out read during a backward search just bounds it.)
            m_action = {Action::Kind::eFailed, 0, 0, notOnCaptureReason()};
            return m_action;
        }
        if (!m_hi || reading.offset < m_hi->offset)
            m_hi = bound;
        if (m_requested == 0 && !m_lo) {
            // The very start of the capture is already at or past the target
            m_action = {Action::Kind::eDone, 0,
                        m_target.isChapter() ? m_hi->value.value_or(m_target.value) : m_target.value};
            return m_action;
        }
        if (m_lo && reading.offset <= m_lo->offset) {
            // Landed at or before the lower bound yet already at the target:
            // the lower bound's reading was the misread one.  Drop it and
            // start over from this side.
            m_lo.reset();
            m_step = seconds(m_target.isChapter() ? m_params.initial_step_seconds : 1.0);
        }
    }
    // A probe that moved neither bound (it landed past the upper bound, the
    // slack being larger than expected) is aimed further short next time;
    // a few in a row and the bracket is as tight as the landing allows
    const bool moved = (m_lo ? m_lo->offset : -1) != lo_before || (m_hi ? m_hi->offset : -1) != hi_before;
    if (!moved) {
        m_landing_bias += seconds(m_params.tolerance_seconds) / 2;
        if (++m_wasted >= 3 && m_lo && m_hi) {
            finish();
            return m_action;
        }
    } else {
        m_wasted = 0;
    }
    decide();
    return m_action;
}

const PositionSearch::Action &PositionSearch::probeReachedEnd() {
    if (m_action.kind != Action::Kind::eSeek && m_action.kind != Action::Kind::eWait)
        return m_action;
    if (m_hi || !m_lo) {
        // Only a forward gallop probes towards the end; anything else that
        // gets there has lost its way
        m_action = {Action::Kind::eFailed, 0, 0, "SEARCH FAILED"};
        return m_action;
    }
    // The last frames of the capture are somewhere before this probe.  One
    // of them that still reads below the target says what a reading at the
    // input's end would: the position is not on this capture.
    m_past_end = true;
    const int64_t next = m_requested - m_back_step;
    m_back_step *= 2;
    if (next <= m_lo->offset) {
        m_action = {Action::Kind::eFailed, 0, 0, notOnCaptureReason()};
        return m_action;
    }
    probe(next);
    return m_action;
}
