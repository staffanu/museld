// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <cmath>
#include <format>
#include "NtscCadenceTracker.h"
#include "NtscConstants.h"
#include "musevk/HalfFloatUtil.h"

namespace {
    // Classification gates as multiples of the adaptive floor, from the Tron
    // capture: repeated fields measure 1.0-1.2x the floor, the weakest real
    // motion ~2.5x.
    constexpr float c_low_gate = 1.4f;
    constexpr float c_high_gate = 2.0f;
    // The floor tracks min(d0, d1) with a fast fall and a slow rise, so
    // sustained motion inflates it only slowly.  The fall is smoothed rather
    // than a hard min: a single outlier-low frame would otherwise pin the
    // floor below the level real repeats measure at, pushing them into the
    // ambiguous band (seen on the dark Tron opening) ...
    constexpr float c_floor_fall = 0.3f;
    constexpr float c_floor_rise = 0.02f;
    constexpr float c_abs_floor = 0.002f;
    // ... and never beyond this multiple of the diff a pure-noise frame pair
    // would measure: mean |8-sample box| of per-sample sigma is
    // sqrt(2 * 8)/8 * sqrt(2/pi) ~= 0.32 sigma, and the real floor carries
    // roughly another 2x of line-locked jitter and wander on top (Tron:
    // predicted 0.0032, measured 0.0067), leaving the cap at 3x predicted
    // still clear of the weakest motion.
    constexpr float c_floor_cap_sigmas = 3.0f * 0.32f;

    // Only observed repeats argue FOR a hypothesis: video content is
    // consistent with every hypothesis on the 3 motion phases out of 5, so
    // motion at a motion phase scores nothing.  Contradictions -- a repeat at
    // the wrong place, or motion where a repeat was expected -- argue against.
    // A weak repeat (the other field merely ambiguous) is worth a third of a
    // clean one and contradicts nothing: in dim or barely moving film scenes
    // it is the dominant evidence (Tron's opening: 35 weak vs 12 clean at the
    // true phase over 20 s), while its phase spread on non-film content is
    // wide enough that the lock margin keeps it from ever locking alone
    // unless the phases agree.
    constexpr float c_repeat_match = 3.0f;
    constexpr float c_weak_repeat_match = 1.0f;
    constexpr float c_contradiction = 4.0f;
    constexpr float c_score_cap = 24.0f;
    constexpr float c_lock_score = 12.0f;  // 4 matched repeats = 2 full cycles
    constexpr float c_lock_margin = 6.0f;
    constexpr float c_unlock_score = 6.0f;

    int phaseOf(int frame_no, int hypothesis) {
        return (frame_no + hypothesis) % 5;
    }
}

NtscCadenceTracker::NtscCadenceTracker(Logger &log)
: m_log(log),
  m_floor(-1.0f),
  m_locked_hypothesis(-1),
  m_consecutive_misses(0),
  m_score{},
  m_classes{} {
}

NtscCadenceTracker::FieldDiffs NtscCadenceTracker::MeasureFieldDiffs(int16_t const *cur, int16_t const *prev) {
    // Sample the active picture: lines 22..261 hold the first field on the
    // 0-based frame buffer, the second field sits 263 lines further down.
    // The box start need not be aligned to the subcarrier -- 8 samples cover
    // two full cycles wherever they begin.
    FieldDiffs result{};
    for (int field = 0; field < 2; field++) {
        double sum = 0;
        long boxes = 0;
        for (int row = 24 + field * 263; row <= 258 + field * 263; row += 3) {
            int16_t const *c = cur + row * NTSC_TOTAL_WIDTH;
            int16_t const *p = prev + row * NTSC_TOTAL_WIDTH;
            for (int x = 152; x + 8 <= 856; x += 12) {
                float box = 0;
                for (int i = 0; i < 8; i++)
                    box += HalfFloatUtil::half_to_float((uint16_t)c[x + i])
                         - HalfFloatUtil::half_to_float((uint16_t)p[x + i]);
                sum += std::abs(box);
                boxes++;
            }
        }
        (field == 0 ? result.d0 : result.d1) = (float)(sum / 8 / boxes);
    }
    return result;
}

NtscCadenceTracker::Level NtscCadenceTracker::classify(float d) const {
    if (d < m_floor * c_low_gate)
        return Level::eLow;
    if (d > m_floor * c_high_gate)
        return Level::eHigh;
    return Level::eAmbiguous;
}

void NtscCadenceTracker::update(int frame_no, float d0, float d1, float noise_sigma_hint) {
    float min01 = std::min(d0, d1);
    if (m_floor < 0)
        m_floor = min01;
    else
        m_floor += (min01 < m_floor ? c_floor_fall : c_floor_rise) * (min01 - m_floor);
    if (noise_sigma_hint > 0)
        m_floor = std::min(m_floor, c_floor_cap_sigmas * noise_sigma_hint);
    m_floor = std::max(m_floor, c_abs_floor);

    Level l0 = classify(d0);
    Level l1 = classify(d1);
    m_classes[frame_no % m_classes.size()] = {frame_no, l0, l1};

    // A missed repeat at the locked phase means the cadence broke there, no
    // matter what the accumulated score still says.  Ambiguous levels touch
    // nothing: borderline noise should not cost a good lock.
    if (m_locked_hypothesis >= 0) {
        int ph = phaseOf(frame_no, m_locked_hypothesis);
        if (ph == 1 || ph == 3) {
            Level expected = ph == 1 ? l0 : l1;
            if (expected == Level::eHigh)
                m_consecutive_misses++;
            else if (expected == Level::eLow)
                m_consecutive_misses = 0;
        }
    }

    bool f0_repeat = l0 == Level::eLow && l1 == Level::eHigh;
    bool f1_repeat = l1 == Level::eLow && l0 == Level::eHigh;
    bool f0_weak = l0 == Level::eLow && l1 == Level::eAmbiguous;
    bool f1_weak = l1 == Level::eLow && l0 == Level::eAmbiguous;
    bool motion = l0 == Level::eHigh && l1 == Level::eHigh;
    if (f0_repeat || f1_repeat || f0_weak || f1_weak || motion) { // static/ambiguous: no information
        for (int h = 0; h < 5; h++) {
            int ph = phaseOf(frame_no, h);
            bool expect_f0 = ph == 1, expect_f1 = ph == 3;
            if ((f0_repeat && expect_f0) || (f1_repeat && expect_f1))
                m_score[h] = std::min(m_score[h] + c_repeat_match, c_score_cap);
            else if ((f0_weak && expect_f0) || (f1_weak && expect_f1))
                m_score[h] = std::min(m_score[h] + c_weak_repeat_match, c_score_cap);
            else if (f0_repeat || f1_repeat || (motion && (expect_f0 || expect_f1)))
                m_score[h] = std::max(m_score[h] - c_contradiction, 0.0f);
            // weak repeats are too uncertain to hold against anyone
        }
    }

    if (m_locked_hypothesis >= 0) {
        float best_other = 0;
        for (int h = 0; h < 5; h++)
            if (h != m_locked_hypothesis)
                best_other = std::max(best_other, m_score[h]);
        char const *reason = m_consecutive_misses >= 2 ? "cadence broke"
                : m_score[m_locked_hypothesis] < c_unlock_score ? "score decayed"
                : best_other > m_score[m_locked_hypothesis] ? "another phase took over"
                : nullptr;
        if (reason != nullptr) {
            m_log.info(eDecoder, std::format("film cadence: unlocked at frame {} ({})", frame_no, reason));
            // A confirmed break restarts acquisition from scratch; keeping
            // the old score would re-lock the stale phase immediately.
            if (m_consecutive_misses >= 2)
                m_score[m_locked_hypothesis] = 0;
            m_locked_hypothesis = -1;
        }
    }

    if (m_log.isEnabled(eDebug, eDecoder))
        m_log.debug(eDecoder, std::format(
                "film cadence: frame {} d0 {:.4f} d1 {:.4f} floor {:.4f} locked {} misses {} scores {:.0f} {:.0f} {:.0f} {:.0f} {:.0f}",
                frame_no, d0, d1, m_floor,
                m_locked_hypothesis >= 0 ? phaseOf(frame_no, m_locked_hypothesis) : -1,
                m_consecutive_misses, m_score[0], m_score[1], m_score[2], m_score[3], m_score[4]));

    if (m_locked_hypothesis < 0) {
        int best = 0;
        for (int h = 1; h < 5; h++)
            if (m_score[h] > m_score[best])
                best = h;
        float second = 0;
        for (int h = 0; h < 5; h++)
            if (h != best)
                second = std::max(second, m_score[h]);
        if (m_score[best] >= c_lock_score && m_score[best] - second >= c_lock_margin) {
            m_locked_hypothesis = best;
            m_consecutive_misses = 0;
            m_log.info(eDecoder, std::format("film cadence: 3:2 lock at frame {}, phase {} (floor {:.4f})",
                                             frame_no, phaseOf(frame_no, best), m_floor));
        }
    }
}

int NtscCadenceTracker::phaseForFrame(int frame_no) const {
    return m_locked_hypothesis < 0 ? -1 : phaseOf(frame_no, m_locked_hypothesis);
}

NtscCadenceTracker::FieldAction NtscCadenceTracker::actionForField(int frame_no, int parity) const {
    if (m_locked_hypothesis < 0)
        return FieldAction::eAdaptive;
    FrameClass const &fc = m_classes[frame_no % m_classes.size()];
    if (fc.frame_no != frame_no)
        return FieldAction::eAdaptive;

    int ph = phaseOf(frame_no, m_locked_hypothesis);
    // On the repeat-carrying phases the frame's own lag-1 difference
    // witnesses the pairing about to be woven; a repeat field that clearly
    // moved vetoes the whole frame back to the motion-adaptive path.  An
    // ambiguous one does not: it bounds any mis-weave at near the noise
    // level, and vetoing on it made "adapt" flicker through dim scenes.
    if ((ph == 1 && fc.l0 == Level::eHigh) || (ph == 3 && fc.l1 == Level::eHigh))
        return FieldAction::eAdaptive;

    static constexpr FieldAction c_actions[5][2] = {
            {FieldAction::eHold,  FieldAction::eWeave}, // A1 A2
            {FieldAction::eWeave, FieldAction::eHold},  // A3 B1
            {FieldAction::eWeave, FieldAction::eHold},  // B2 C1
            {FieldAction::eWeave, FieldAction::eWeave}, // C2 C3
            {FieldAction::eHold,  FieldAction::eWeave}, // D1 D2
    };
    return c_actions[ph][parity];
}
