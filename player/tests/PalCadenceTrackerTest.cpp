// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <catch2/catch_all.hpp>
#include <vector>
#include "sdtv/PalCadenceTracker.h"
#include "musevk/HalfFloatUtil.h"

using FA = PalCadenceTracker::FieldAction;

namespace {
    struct NullLogger : Logger {
        void log(LogPriority, LogCategoryFlags, const std::string &) override {}
        [[nodiscard]] bool isEnabled(LogPriority, LogCategoryFlags) const override { return false; }
        void sync() override {}
    };

    constexpr float c_floor = 0.006f;  // the right pairing: vertical detail and noise
    constexpr float c_motion = 0.03f;  // the wrong pairing on a moving frame

    // A moving film frame in the given phase: 0 = F1+F2 of the frame
    void feedFilm(PalCadenceTracker &t, int f, int phase) {
        t.update(f, phase == 0 ? c_floor : c_motion, phase == 0 ? c_motion : c_floor, -1.0f);
    }
    void feedStill(PalCadenceTracker &t, int f) { t.update(f, c_floor, c_floor, -1.0f); }
    void feedVideo(PalCadenceTracker &t, int f) { t.update(f, c_motion, c_motion * 0.9f, -1.0f); }
}

TEST_CASE("2:2 film locks after five moving frames and pairs the fields by phase") {
    NullLogger log;
    for (int phase = 0; phase < 2; phase++) {
        PalCadenceTracker t(log);
        int f = 1;
        for (; f <= 4; f++) {
            feedFilm(t, f, phase);
            CHECK_FALSE(t.isLocked());
        }
        feedFilm(t, f, phase);
        CHECK(t.isLocked());
        CHECK(t.phaseForFrame(f) == phase);
        // phase 0: field 1 starts the film frame (hold), field 2 completes it
        // (weave); phase 1 the other way round
        CHECK(t.actionForField(f, 0) == (phase == 0 ? FA::eHold : FA::eWeave));
        CHECK(t.actionForField(f, 1) == (phase == 0 ? FA::eWeave : FA::eHold));
        // frames not fed are adaptive
        CHECK(t.actionForField(f + 1, 0) == FA::eAdaptive);
        CHECK(t.actionForField(f - 20, 0) == FA::eAdaptive);
    }
}

TEST_CASE("stills carry no evidence: no lock from them and a lock survives them") {
    NullLogger log;
    PalCadenceTracker t(log);
    int f = 1;
    for (; f <= 40; f++)
        feedStill(t, f);
    CHECK_FALSE(t.isLocked());
    for (int i = 0; i < 5; i++, f++)
        feedFilm(t, f, 1);
    REQUIRE(t.isLocked());
    for (int i = 0; i < 100; i++, f++) {
        feedStill(t, f);
        CHECK(t.isLocked());
        CHECK(t.actionForField(f, 0) == FA::eWeave);
    }
}

TEST_CASE("video never locks and unlocks a lock within two frames") {
    NullLogger log;
    PalCadenceTracker t(log);
    int f = 1;
    for (; f <= 60; f++) {
        feedVideo(t, f);
        CHECK_FALSE(t.isLocked());
    }
    // the floor is still falling from the video level: a few more frames
    // than from stills
    for (int i = 0; i < 8; i++, f++)
        feedFilm(t, f, 0);
    REQUIRE(t.isLocked());
    feedVideo(t, f);
    CHECK(t.actionForField(f, 1) == FA::eAdaptive); // the contradicting frame itself is not woven
    f++;
    feedVideo(t, f);
    CHECK_FALSE(t.isLocked());
}

TEST_CASE("cuts between frames on still video never add up to a lock") {
    NullLogger log;
    PalCadenceTracker t(log);
    int f = 1;
    for (int cut = 0; cut < 20; cut++) {
        for (int i = 0; i < 30; i++, f++)
            feedStill(t, f);
        t.update(f++, c_floor, 0.3f, -1.0f); // the across pairing straddles a cut
        CHECK_FALSE(t.isLocked());
    }
}

TEST_CASE("a cut on an odd field flips the phase: unlock then re-lock in the new phase") {
    NullLogger log;
    PalCadenceTracker t(log);
    int f = 1;
    for (; f <= 10; f++)
        feedFilm(t, f, 0);
    REQUIRE(t.phaseForFrame(f - 1) == 0);
    int unlocked_at = -1, relocked_at = -1;
    for (int i = 0; i < 12; i++, f++) {
        feedFilm(t, f, 1);
        if (!t.isLocked() && unlocked_at < 0) unlocked_at = i;
        if (t.isLocked() && unlocked_at >= 0 && relocked_at < 0 && t.phaseForFrame(f) == 1) relocked_at = i;
    }
    CHECK(unlocked_at >= 0);
    CHECK(unlocked_at <= 2);
    CHECK(relocked_at >= 0);
    CHECK(relocked_at <= 9);
}

TEST_CASE("one noisy contradiction does not break a lock") {
    NullLogger log;
    PalCadenceTracker t(log);
    int f = 1;
    for (; f <= 10; f++)
        feedFilm(t, f, 0);
    REQUIRE(t.isLocked());
    feedVideo(t, f++);
    CHECK(t.isLocked());
    feedFilm(t, f++, 0);
    CHECK(t.isLocked());
}

TEST_CASE("MeasureCombs prefers the pairing whose fields match") {
    // Two frames of a moving vertical ramp: field 2 of frame N sits halfway
    // between frame N's field 1 lines (phase 0), and the previous frame's
    // field 2 is shifted by half a line pitch more (motion between frames)
    const int W = PalCadenceTracker::c_frame_width, H = PalCadenceTracker::c_frame_height;
    std::vector<int16_t> cur(W * H), prev(W * H);
    auto line_value = [](int line_half) { return 0.002f * line_half; }; // ramp in half-line units
    for (int k = 0; k < PalCadenceTracker::c_field_height; k++)
        for (int x = 0; x < W; x++) {
            const int r1 = PalCadenceTracker::c_field_start_y + k;
            const int r2 = r1 + PalCadenceTracker::c_field2_offset;
            cur[r1 * W + x] = (int16_t)HalfFloatUtil::float_to_half(line_value(2 * k));
            cur[r2 * W + x] = (int16_t)HalfFloatUtil::float_to_half(line_value(2 * k + 1));
            prev[r1 * W + x] = (int16_t)HalfFloatUtil::float_to_half(line_value(2 * k - 6));
            prev[r2 * W + x] = (int16_t)HalfFloatUtil::float_to_half(line_value(2 * k - 5));
        }
    auto c = PalCadenceTracker::MeasureCombs(cur.data(), prev.data());
    CHECK(c.same < 0.0005f);
    CHECK(c.across > 0.005f);
}
