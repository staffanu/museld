// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_PALCADENCETRACKER_H
#define MUSECPP_PALCADENCETRACKER_H

#include <array>
#include <cstdint>
#include "CadenceAction.h"
#include "logging/Logger.h"

// Detects the 2:2 cadence of film-sourced PAL content and answers, per
// displayed field, how the de-interlacer should pair it.  Film at 24
// frames/s runs at 25 on PAL, one film frame per video frame with no
// repeated field, so unlike NTSC's 3:2 there is no content-independent
// anchor: the only evidence is that one of the two adjacent-field pairings
// weaves cleanly and the other combs, and it exists only while something
// moves.  The two candidates for a frame N are its own fields, F1(N)+F2(N)
// ("phase 0"), and the previous frame's second field with this frame's
// first, F2(N-1)+F1(N) ("phase 1"); which one a transfer uses depends on
// how the telecine was synced, and a cut on an odd field flips it.
//
// The measure is the combing of each candidate weave: the deviation of the
// field-2 line from the mean of the field-1 lines around it, through an
// 8-sample box that nulls the subcarrier, summed over the picture.  A right
// pairing combs only with vertical detail and noise, a wrong one with the
// motion between its two fields; video-sourced material combs in both.
class PalCadenceTracker {
public:
    using FieldAction = CadenceFieldAction;

    struct Combs {
        float same;   // F1(N) woven with F2(N)
        float across; // F2(N-1) woven with F1(N)
    };

    // The PAL frame buffer this tracker measures: 1135 x 625, field 1 from
    // row 23, field 2 313 rows further down, line k of field 2 between
    // lines k and k + 1 of field 1
    static constexpr int c_frame_width = 1135;
    static constexpr int c_frame_height = 625;
    static constexpr int c_field_start_y = 23;
    static constexpr int c_field2_offset = 313;
    static constexpr int c_field_height = 288;
    static constexpr int c_picture_x0 = 180;
    static constexpr int c_picture_x1 = 1124;

    // The combing of the two candidate pairings, on two half-float frame
    // buffers, the frame just read and its predecessor
    static Combs MeasureCombs(int16_t const *cur, int16_t const *prev);

    explicit PalCadenceTracker(Logger &log);

    // Feed the combs of the newly read frame `frame_no`, once per frame in
    // reading order.  noise_sigma_hint as for NtscCadenceTracker.
    void update(int frame_no, float same, float across, float noise_sigma_hint);

    // The action for one field of a frame covered by a recent update()
    // (anything else returns eAdaptive)
    [[nodiscard]] FieldAction actionForField(int frame_no, int parity) const;

    [[nodiscard]] bool isLocked() const { return m_locked_phase >= 0; }

    // The locked pairing, 0 or 1 as above, -1 when unlocked
    [[nodiscard]] int phaseForFrame(int frame_no) const;

private:
    enum class Vote { eNone, ePhase0, ePhase1, eVideo };
    struct FrameVote {
        int frame_no = -1;
        Vote vote = Vote::eNone;
    };

    Logger &m_log;
    float m_floor;            // adaptive floor of the lower comb: vertical detail plus noise
    int m_locked_phase;       // -1 when unlocked
    int m_consecutive_misses; // moving frames that contradicted the lock
    bool m_prev_moving;       // the previous frame carried motion
    std::array<float, 2> m_score;
    std::array<FrameVote, 8> m_votes; // ring, indexed by frame_no % size
};

#endif //MUSECPP_PALCADENCETRACKER_H
