// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "ChapterSearch.h"

#include <algorithm>
#include <cmath>

ChapterSearch::ChapterSearch(const Params &params, const Reading &current, Direction direction)
        : m_params(params),
          m_direction(direction),
          m_origin(current),
          m_target(0),
          m_step(seconds(params.initial_step_seconds)),
          m_requested(current.offset),
          // Frames land a little after the requested position: the input
          // reader's block granularity and the first whole frame after the
          // seek.  Two frames to start with; measured from then on.
          m_landing_bias(seconds(2.0 / 30.0)),
          m_frames_without_chapter(0),
          m_retries_without_chapter(0),
          m_probes(0),
          m_wasted(0),
          m_chained(false),
          m_action{Action::Kind::eFailed} {
    if (!current.chapter.has_value()) {
        m_action = {Action::Kind::eFailed, 0, 0, "NO CHAPTER CODE"};
        return;
    }
    if (params.input_sample_count < 0 && direction == Direction::eNext) {
        // Probing past the end of the input finishes the decoding threads
        // for good, so forward probes need to know where the end is
        m_action = {Action::Kind::eFailed, 0, 0, "INPUT LENGTH UNKNOWN"};
        return;
    }
    if (direction == Direction::eNext) {
        m_target = *current.chapter + 1;
        m_lo = Bound{current.offset, current.chapter};
    } else {
        // The start of the current chapter first; decide() moves on to the
        // one before when that turns out to be right behind us
        m_target = *current.chapter;
        m_hi = Bound{current.offset, current.chapter};
    }
    decide();
}

ChapterSearch::ChapterSearch(const Params &params, const Reading &current, int target_chapter)
        : ChapterSearch(params, current,
                        current.chapter && *current.chapter >= target_chapter ? Direction::ePrevious
                                                                              : Direction::eNext) {
    if (m_action.kind == Action::Kind::eFailed)
        return;
    m_target = target_chapter;
    m_chained = true; // an explicit target never moves on to the chapter before
    if (*current.chapter == target_chapter) {
        // Already inside it: look for its start
        m_hi = Bound{current.offset, current.chapter};
        m_lo.reset();
    }
    m_step = seconds(params.initial_step_seconds);
    m_probes = 0;
    decide();
}

int64_t ChapterSearch::clampToInput(int64_t offset) const {
    offset = std::max<int64_t>(0, offset);
    if (m_params.input_sample_count >= 0) {
        // A second short of the end: the decoder needs a whole frame there
        offset = std::min(offset, std::max<int64_t>(0, m_params.input_sample_count - seconds(1.0)));
    }
    return offset;
}

void ChapterSearch::probe(int64_t offset) {
    m_requested = clampToInput(offset);
    m_frames_without_chapter = 0;
    m_probes++;
    m_action = {Action::Kind::eSeek, m_requested};
}

void ChapterSearch::finish() {
    // The chapter is known to have started by m_hi
    const int64_t start = m_hi->offset;
    if (m_direction == Direction::ePrevious && !m_chained
        && m_origin.offset - start < seconds(m_params.restart_slack_seconds)
        && m_target > 0 && m_target - 1 >= 0) {
        // Already at the start of this chapter: the previous one is meant.
        // The bracket is known on one side: this chapter's start is at or
        // past the previous chapter's end.
        m_chained = true;
        m_target--;
        m_hi = Bound{start, m_target + 1};
        m_lo.reset();
        m_step = seconds(m_params.initial_step_seconds);
        decide();
        return;
    }
    m_action = {Action::Kind::eDone, start, m_hi->chapter.value_or(m_target)};
}

void ChapterSearch::decide() {
    const int64_t tolerance = seconds(m_params.tolerance_seconds);

    if (m_lo && m_hi) {
        if (m_hi->offset - m_lo->offset <= tolerance) {
            finish();
            return;
        }
        // Between the chapters read at the two bounds lie (hi - lo) chapter
        // starts; assuming them evenly spaced puts the target's start at a
        // definite fraction of the bracket.  With adjacent chapters at the
        // bounds this is the midpoint, i.e. plain bisection.  Guessing too
        // close to a bound would waste a probe on the landing slack, so the
        // guess stays inside the middle 80 % of the bracket.
        const int64_t span = m_hi->offset - m_lo->offset;
        double fraction = 0.5;
        if (m_lo->chapter && m_hi->chapter && *m_hi->chapter > *m_lo->chapter)
            fraction = (double)(m_target - *m_lo->chapter) / (double)(*m_hi->chapter - *m_lo->chapter + 1);
        fraction = std::clamp(fraction, 0.1, 0.9);
        const int64_t guess = m_lo->offset + (int64_t)(fraction * (double)span);
        // Aim short of the guess by the measured landing slack, and never
        // at or before the lower bound (whose frame we would read again)
        probe(std::max(m_lo->offset + 1, guess - m_landing_bias));
        return;
    }

    if (m_lo) {
        // Forward: gallop from the lower bound until the target is passed.
        // The input end is a bound on its own: a probe clamped there that
        // still reads below the target means the chapter is not on this
        // capture, handled in feed().
        probe(m_lo->offset + m_step);
        m_step *= 2;
        return;
    }

    // Backward: gallop from the upper bound towards the start
    if (m_hi->offset == 0) {
        // The chapter started before the capture did
        m_action = {Action::Kind::eDone, 0, m_hi->chapter.value_or(m_target)};
        return;
    }
    probe(m_hi->offset - m_step);
    m_step *= 2;
}

const ChapterSearch::Action &ChapterSearch::feed(const Reading &reading) {
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

    if (!reading.chapter && !reading.lead_in && !reading.lead_out) {
        if (++m_frames_without_chapter < m_params.frames_without_chapter_limit) {
            m_action = {Action::Kind::eWait};
            return m_action;
        }
        // Nothing to read here: a stretch without codes, or a disc without
        // chapters at all.  Nudge the probe forward a little, a few times.
        if (++m_retries_without_chapter > 3) {
            m_action = {Action::Kind::eFailed, 0, 0, "NO CHAPTER CODE"};
            return m_action;
        }
        probe(m_requested + seconds(1.0));
        return m_action;
    }

    // Which side of the target is this frame on?
    const bool below = reading.lead_in || (reading.chapter && *reading.chapter < m_target && !reading.lead_out);
    const Bound bound{reading.offset, reading.lead_in || reading.lead_out ? std::nullopt : reading.chapter};
    const auto lo_before = m_lo ? m_lo->offset : -1;
    const auto hi_before = m_hi ? m_hi->offset : -1;
    if (below) {
        if (m_hi && reading.offset >= m_hi->offset) {
            // Landed past the upper bound and still below the target: the
            // disc's codes contradict themselves here (a dropout misread,
            // or chapters out of order); accept the upper bound as the start
            m_action = {Action::Kind::eDone, m_hi->offset, m_hi->chapter.value_or(m_target)};
            return m_action;
        }
        const bool at_input_end = m_params.input_sample_count >= 0
                && m_requested >= clampToInput(m_params.input_sample_count);
        if (at_input_end && m_direction == Direction::eNext && !m_hi) {
            m_action = {Action::Kind::eFailed, 0, 0, "NO MORE CHAPTERS"};
            return m_action;
        }
        if (!m_lo || reading.offset > m_lo->offset)
            m_lo = bound;
    } else {
        if (reading.lead_out && m_direction == Direction::eNext && !m_hi) {
            // Beyond the programme: the next chapter does not exist.  (A
            // lead-out read during a backward search just bounds it.)
            m_action = {Action::Kind::eFailed, 0, 0, "NO MORE CHAPTERS"};
            return m_action;
        }
        if (!m_hi || reading.offset < m_hi->offset)
            m_hi = bound;
        if (m_requested == 0 && !m_lo) {
            // The very start of the capture is already in the target chapter
            m_action = {Action::Kind::eDone, 0, m_hi->chapter.value_or(m_target)};
            return m_action;
        }
        if (m_lo && reading.offset <= m_lo->offset) {
            // Landed at or before the lower bound yet already in the target:
            // the lower bound's reading was the misread one.  Drop it and
            // start over from this side.
            m_lo.reset();
            m_step = seconds(m_params.initial_step_seconds);
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
