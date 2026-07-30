// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_NTSCCADENCETRACKER_H
#define MUSECPP_NTSCCADENCETRACKER_H

#include <array>
#include <cstdint>
#include "logging/Logger.h"

// Detects the 3:2 pulldown cadence of film-sourced content from the frame
// data itself and answers, per displayed field, how the de-interlacer should
// pair it.  The disc metadata is no help: the white flag (IEC 60857 line 11,
// nominally "first field of a film frame") is written on every frame
// regardless of cadence on real discs, so the only trustworthy evidence is
// the repeated field -- under 3:2, one field in five equals the same-parity
// field of the previous frame.
//
// Frames are grouped (A1A2)(A3B1)(B2C1)(C2C3)(D1D2) for film frames A..D;
// the "phase" of a frame is its position 0..4 in that cycle.  The repeats
// are A3==A1 (phase 1, field 0) and C3==C1 (phase 3, field 1).
class NtscCadenceTracker {
public:
    // What the de-interlacer should do with a displayed field:
    //   eAdaptive: no lock, or evidence against it -- per-pixel motion masks.
    //   eWeave:    the previously decoded field belongs to the same film
    //              frame; weave it in unconditionally.
    //   eHold:     the field starts a new film frame whose other half is not
    //              decoded yet; show the previous film frame once more (the
    //              weave of the two previously decoded fields).
    enum class FieldAction { eAdaptive, eWeave, eHold };

    struct FieldDiffs {
        float d0; // field 0 mean |difference| against the previous frame
        float d1;
    };

    // Mean absolute difference per field between two 910x525 half-float
    // frame buffers, through an 8-sample box whose nulls sit at fsc and its
    // harmonics on the 4 fsc grid: the subcarrier inverts between adjacent
    // frames, so a plain difference of still colored content would be twice
    // the chroma (same trick as ntsc_detect_motion.comp).
    static FieldDiffs MeasureFieldDiffs(int16_t const *cur, int16_t const *prev);

    explicit NtscCadenceTracker(Logger &log);

    // Feed the diffs between the newly read frame `frame_no` and its
    // predecessor, once per frame in reading order.  noise_sigma_hint is the
    // expected per-sample noise sigma of the frame data (after de-emphasis
    // and rescale), or <= 0 when unknown; it bounds the adaptive floor so
    // sustained motion cannot inflate it into classifying weak motion as
    // repeats.
    void update(int frame_no, float d0, float d1, float noise_sigma_hint);

    // The action for one field of a frame covered by a recent update()
    // (anything else returns eAdaptive).
    [[nodiscard]] FieldAction actionForField(int frame_no, int parity) const;

    [[nodiscard]] bool isLocked() const { return m_locked_hypothesis >= 0; }

    // The locked phase 0..4 of the frame, -1 when unlocked
    [[nodiscard]] int phaseForFrame(int frame_no) const;

private:
    enum class Level { eLow, eHigh, eAmbiguous };

    [[nodiscard]] Level classify(float d) const;

    struct FrameClass {
        int frame_no = -1;
        Level l0 = Level::eAmbiguous;
        Level l1 = Level::eAmbiguous;
    };

    Logger &m_log;
    float m_floor;             // adaptive repeat/noise floor, -1 until seeded
    int m_locked_hypothesis;   // h with phase(f) = (f + h) % 5; -1 when unlocked
    int m_consecutive_misses;  // expected repeats that failed to materialize
    std::array<float, 5> m_score;
    std::array<FrameClass, 8> m_classes; // ring, indexed by frame_no % size
};

#endif //MUSECPP_NTSCCADENCETRACKER_H
