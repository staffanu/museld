// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <cmath>
#include <format>
#include "PalCadenceTracker.h"
#include "musevk/HalfFloatUtil.h"

namespace {
    // Both combs carry the floor -- vertical detail and noise, the same in
    // either pairing -- so the evidence is in their excess over it.  A
    // frame carries evidence only when the higher excess stands clear of
    // the floor: below that nothing moved and both pairings weave alike.
    constexpr float c_motion_gate = 0.6f;
    // Film: the lower excess is at most this fraction of the higher (the
    // right pairing combs with vertical detail only, the wrong one with the
    // motion on top).  Video: both pairings comb with motion, comparably.
    // Between the two the frame says nothing.
    constexpr float c_film_ratio = 0.35f;
    constexpr float c_video_ratio = 0.7f;
    // The floor tracks the lower comb with a fast fall and a slower rise,
    // and never beyond this multiple of what noise alone would measure: the
    // comb of a field-2 line against the mean of two field-1 lines has
    // variance 1.5 sigma^2 / 8 through the box, mean absolute value ~0.35
    // sigma, with vertical detail on top.
    constexpr float c_floor_fall = 0.3f;
    constexpr float c_floor_rise = 0.05f;
    constexpr float c_abs_floor = 0.002f;
    constexpr float c_floor_cap_sigmas = 3.0f * 0.35f;
    // Lock slow, unlock fast: a wrong lock weaves two instants together,
    // which is worse than the adaptive path.  A film vote counts only with
    // motion in the frame before it as well: a cut is a one-frame comb in
    // whichever pairing straddles it, and on still video four cuts would
    // otherwise add up to a lock (The Big Day).  Unlocked, the scores decay
    // between votes, so only sustained motion locks; locked, stills keep
    // the lock, and two contradicting moving frames in a row drop it.
    constexpr float c_vote = 3.0f;
    constexpr float c_contradiction = 4.0f;
    constexpr float c_quiet_decay = 0.8f;
    constexpr float c_score_cap = 24.0f;
    constexpr float c_lock_score = 12.0f;
    constexpr float c_lock_margin = 6.0f;
    constexpr float c_unlock_score = 6.0f;
}

PalCadenceTracker::Combs PalCadenceTracker::MeasureCombs(int16_t const *cur, int16_t const *prev) {
    // Sample every third line pair and every twelfth column of the picture;
    // 8-sample boxes cover two subcarrier cycles wherever they begin
    double sum_same = 0, sum_across = 0;
    long boxes = 0;
    auto box = [](int16_t const *p) {
        float s = 0;
        for (int i = 0; i < 8; i++)
            s += HalfFloatUtil::half_to_float((uint16_t)p[i]);
        return s;
    };
    for (int k = 2; k + 1 < c_field_height - 2; k += 3) {
        int16_t const *a1 = cur + (size_t)(c_field_start_y + k) * c_frame_width;
        int16_t const *a2 = a1 + c_frame_width;
        int16_t const *b_same = cur + (size_t)(c_field_start_y + c_field2_offset + k) * c_frame_width;
        int16_t const *b_across = prev + (size_t)(c_field_start_y + c_field2_offset + k) * c_frame_width;
        for (int x = c_picture_x0; x + 8 <= c_picture_x1; x += 12) {
            const float mean_a = 0.5f * (box(a1 + x) + box(a2 + x));
            sum_same += std::abs(box(b_same + x) - mean_a);
            sum_across += std::abs(box(b_across + x) - mean_a);
            boxes++;
        }
    }
    return {(float)(sum_same / 8 / boxes), (float)(sum_across / 8 / boxes)};
}

PalCadenceTracker::PalCadenceTracker(Logger &log)
: m_log(log),
  m_floor(-1.0f),
  m_locked_phase(-1),
  m_consecutive_misses(0),
  m_prev_moving(false),
  m_score{},
  m_votes{} {
}

void PalCadenceTracker::update(int frame_no, float same, float across, float noise_sigma_hint) {
    const float lo = std::min(same, across), hi = std::max(same, across);
    if (m_floor < 0)
        m_floor = lo;
    else
        m_floor += (lo < m_floor ? c_floor_fall : c_floor_rise) * (lo - m_floor);
    if (noise_sigma_hint > 0)
        m_floor = std::min(m_floor, c_floor_cap_sigmas * noise_sigma_hint);
    m_floor = std::max(m_floor, c_abs_floor);

    const float ex_lo = std::max(lo - m_floor, 0.0f), ex_hi = std::max(hi - m_floor, 0.0f);
    const bool moving = ex_hi > m_floor * c_motion_gate;
    Vote vote = Vote::eNone;
    if (moving) {
        if (ex_lo <= ex_hi * c_film_ratio)
            vote = m_prev_moving ? (same < across ? Vote::ePhase0 : Vote::ePhase1) : Vote::eNone;
        else if (ex_lo >= ex_hi * c_video_ratio)
            vote = Vote::eVideo;
    }
    m_prev_moving = moving;
    m_votes[frame_no % m_votes.size()] = {frame_no, vote};

    if (vote == Vote::eNone && m_locked_phase < 0)
        for (float &s : m_score)
            s *= c_quiet_decay;
    if (vote != Vote::eNone) {
        for (int p = 0; p < 2; p++) {
            const bool mine = vote == (p == 0 ? Vote::ePhase0 : Vote::ePhase1);
            m_score[p] = mine ? std::min(m_score[p] + c_vote, c_score_cap)
                              : std::max(m_score[p] - c_contradiction, 0.0f);
        }
        if (m_locked_phase >= 0) {
            const bool supports = vote == (m_locked_phase == 0 ? Vote::ePhase0 : Vote::ePhase1);
            m_consecutive_misses = supports ? 0 : m_consecutive_misses + 1;
        }
    }

    if (m_locked_phase >= 0) {
        const int other = 1 - m_locked_phase;
        char const *reason = m_consecutive_misses >= 2 ? "cadence broke"
                : m_score[m_locked_phase] < c_unlock_score ? "score decayed"
                : m_score[other] >= m_score[m_locked_phase] + c_lock_margin ? "the other pairing took over"
                : nullptr;
        if (reason != nullptr) {
            m_log.info(eDecoder, std::format("film cadence: 2:2 unlocked at frame {} ({})", frame_no, reason));
            if (m_consecutive_misses >= 2)
                m_score[m_locked_phase] = 0;
            m_locked_phase = -1;
            m_consecutive_misses = 0;
        }
    }

    if (m_log.isEnabled(eDebug, eDecoder))
        m_log.debug(eDecoder, std::format(
                "film cadence: frame {} comb same {:.4f} across {:.4f} floor {:.4f} vote {} locked {} misses {} scores {:.0f} {:.0f}",
                frame_no, same, across, m_floor,
                vote == Vote::eNone ? "-" : vote == Vote::ePhase0 ? "F1+F2" : vote == Vote::ePhase1 ? "F2+F1" : "video",
                m_locked_phase, m_consecutive_misses, m_score[0], m_score[1]));

    if (m_locked_phase < 0) {
        const int best = m_score[1] > m_score[0] ? 1 : 0;
        if (m_score[best] >= c_lock_score && m_score[best] >= m_score[1 - best] + c_lock_margin) {
            m_locked_phase = best;
            m_consecutive_misses = 0;
            m_log.info(eDecoder, std::format("film cadence: 2:2 locked at frame {}, film frames are {}",
                                             frame_no, best == 0 ? "F1+F2 of a frame" : "F2 of a frame + F1 of the next"));
        }
    }
}

int PalCadenceTracker::phaseForFrame(int frame_no) const {
    const FrameVote &v = m_votes[frame_no % m_votes.size()];
    return m_locked_phase >= 0 && v.frame_no == frame_no ? m_locked_phase : -1;
}

PalCadenceTracker::FieldAction PalCadenceTracker::actionForField(int frame_no, int parity) const {
    const int phase = phaseForFrame(frame_no);
    if (phase < 0)
        return FieldAction::eAdaptive;
    // A frame that itself contradicted the lock (a cut on an odd field, a
    // video insert) is not woven on the lock's say-so
    const Vote v = m_votes[frame_no % m_votes.size()].vote;
    if (v == Vote::eVideo || v == (phase == 0 ? Vote::ePhase1 : Vote::ePhase0))
        return FieldAction::eAdaptive;
    // Phase 0: field 1 starts the film frame (hold), field 2 completes it
    // (weave).  Phase 1: field 1 completes the film frame begun by the
    // previous frame's field 2 (weave), field 2 starts the next (hold).
    return parity == phase ? FieldAction::eHold : FieldAction::eWeave;
}
