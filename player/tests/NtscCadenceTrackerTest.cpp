// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <catch2/catch_all.hpp>
#include <vector>
#include "ntsc/NtscCadenceTracker.h"
#include "ntsc/NtscConstants.h"
#include "musevk/HalfFloatUtil.h"

using FA = NtscCadenceTracker::FieldAction;

namespace {
    struct NullLogger : Logger {
        void log(LogPriority, LogCategoryFlags, const std::string &) override {}
        [[nodiscard]] bool isEnabled(LogPriority, LogCategoryFlags) const override { return false; }
        void sync() override {}
    };

    constexpr float c_floor = 0.007f;  // measured repeat/noise level on Tron
    constexpr float c_motion = 0.03f;  // typical real-motion level

    // One frame of a 3:2 sequence whose ground-truth phase is (f + shift) % 5:
    // the f0 repeat (A3 == A1) lands on phase 1, the f1 repeat (C3 == C1) on
    // phase 3
    void feedPulldownFrame(NtscCadenceTracker &t, int f, int shift = 0) {
        t.update(f, (f + shift) % 5 == 1 ? c_floor : c_motion,
                 (f + shift) % 5 == 3 ? c_floor : c_motion, -1.0f);
    }

    constexpr FA c_expected_actions[5][2] = {
            {FA::eHold,  FA::eWeave}, // A1 A2
            {FA::eWeave, FA::eHold},  // A3 B1
            {FA::eWeave, FA::eHold},  // B2 C1
            {FA::eWeave, FA::eWeave}, // C2 C3
            {FA::eHold,  FA::eWeave}, // D1 D2
    };
}

TEST_CASE("locks onto a 3:2 cadence and maps the film actions") {
    NullLogger log;
    NtscCadenceTracker t(log);
    for (int f = 2; f < 30; f++)
        feedPulldownFrame(t, f);
    REQUIRE(t.isLocked());
    for (int f = 25; f < 30; f++) {
        REQUIRE(t.phaseForFrame(f) == f % 5);
        for (int p = 0; p < 2; p++)
            REQUIRE(t.actionForField(f, p) == c_expected_actions[f % 5][p]);
    }
    // Frames never fed (or long gone from the ring) get the safe default
    REQUIRE(t.actionForField(1000, 0) == FA::eAdaptive);
}

TEST_CASE("video motion never locks") {
    NullLogger log;
    NtscCadenceTracker t(log);
    for (int f = 2; f < 200; f++)
        t.update(f, c_motion, c_motion, -1.0f);
    REQUIRE_FALSE(t.isLocked());
    REQUIRE(t.actionForField(199, 0) == FA::eAdaptive);
    REQUIRE(t.phaseForFrame(199) == -1);
}

TEST_CASE("static scenes carry no information and never lock") {
    NullLogger log;
    NtscCadenceTracker t(log);
    for (int f = 2; f < 200; f++)
        t.update(f, c_floor, c_floor, -1.0f);
    REQUIRE_FALSE(t.isLocked());
}

TEST_CASE("a lock survives a static stretch") {
    NullLogger log;
    NtscCadenceTracker t(log);
    for (int f = 2; f < 30; f++)
        feedPulldownFrame(t, f);
    REQUIRE(t.isLocked());
    for (int f = 30; f < 300; f++) {
        t.update(f, c_floor, c_floor, -1.0f);
        REQUIRE(t.isLocked());
    }
    // Static frames weave/hold like any other locked frame (a repeat is
    // trivially present)
    REQUIRE(t.actionForField(299, 0) == c_expected_actions[299 % 5][0]);
    for (int f = 300; f < 320; f++)
        feedPulldownFrame(t, f);
    REQUIRE(t.isLocked());
}

TEST_CASE("a single cut neither breaks the lock nor gets woven") {
    NullLogger log;
    NtscCadenceTracker t(log);
    for (int f = 2; f < 30; f++)
        feedPulldownFrame(t, f);
    REQUIRE(t.isLocked());
    t.update(30, c_motion, c_motion, -1.0f); // phase 0: a normal motion frame
    t.update(31, 10 * c_motion, 10 * c_motion, -1.0f); // cut where the f0 repeat was due
    REQUIRE(t.isLocked());
    // The missing repeat vetoes this frame's weave, but only this frame's
    REQUIRE(t.actionForField(31, 0) == FA::eAdaptive);
    REQUIRE(t.actionForField(31, 1) == FA::eAdaptive);
    for (int f = 32; f < 40; f++)
        feedPulldownFrame(t, f);
    REQUIRE(t.isLocked());
    REQUIRE(t.actionForField(36, 0) == FA::eWeave); // phase 1 carries its repeat again
}

TEST_CASE("a cadence break unlocks within two missed repeats") {
    NullLogger log;
    NtscCadenceTracker t(log);
    for (int f = 2; f < 30; f++)
        feedPulldownFrame(t, f);
    REQUIRE(t.isLocked());
    int unlocked_at = -1;
    for (int f = 30; f < 45; f++) {
        t.update(f, c_motion, c_motion, -1.0f);
        if (!t.isLocked()) {
            unlocked_at = f;
            break;
        }
    }
    // The expected repeats after frame 29 sit on frames 31 (phase 1) and 33
    // (phase 3); the second miss must end the lock
    REQUIRE(unlocked_at == 33);
    REQUIRE(t.actionForField(unlocked_at, 0) == FA::eAdaptive);
}

TEST_CASE("re-locks to a shifted cadence after a break") {
    NullLogger log;
    NtscCadenceTracker t(log);
    for (int f = 2; f < 30; f++)
        feedPulldownFrame(t, f);
    REQUIRE(t.isLocked());
    for (int f = 30; f < 36; f++)
        t.update(f, c_motion, c_motion, -1.0f);
    REQUIRE_FALSE(t.isLocked());
    for (int f = 36; f < 70; f++)
        feedPulldownFrame(t, f, 2);
    REQUIRE(t.isLocked());
    REQUIRE(t.actionForField(69, 0) == c_expected_actions[(69 + 2) % 5][0]);
    REQUIRE(t.actionForField(69, 1) == c_expected_actions[(69 + 2) % 5][1]);
}

TEST_CASE("weak repeats lock a dim low-motion film scene") {
    NullLogger log;
    NtscCadenceTracker t(log);
    // The moving field never clears the high gate (2x floor): every repeat
    // observation is weak, worth a third of a clean one
    for (int f = 2; f < 70; f++)
        t.update(f, f % 5 == 1 ? c_floor : 1.7f * c_floor,
                 f % 5 == 3 ? c_floor : 1.7f * c_floor, -1.0f);
    REQUIRE(t.isLocked());
    REQUIRE(t.actionForField(69, 0) == c_expected_actions[69 % 5][0]);
}

TEST_CASE("an ambiguous repeat field does not veto a locked frame") {
    NullLogger log;
    NtscCadenceTracker t(log);
    for (int f = 2; f < 30; f++)
        feedPulldownFrame(t, f);
    REQUIRE(t.isLocked());
    // Frame 31 carries the f0 repeat; it measures in the ambiguous band
    t.update(30, c_motion, c_motion, -1.0f);
    t.update(31, 1.6f * c_floor, c_motion, -1.0f);
    REQUIRE(t.actionForField(31, 0) == FA::eWeave);
    REQUIRE(t.actionForField(31, 1) == c_expected_actions[1][1]);
}

TEST_CASE("weak repeats at wandering phases never lock") {
    NullLogger log;
    NtscCadenceTracker t(log);
    // Low-motion video whose quieter field dips into the weak-repeat band at
    // pseudo-random frames: evidence spreads across the hypotheses and the
    // lock margin never opens
    unsigned rng = 12345;
    for (int f = 2; f < 500; f++) {
        rng = rng * 1103515245 + 12345;
        bool dip = (rng >> 16) % 3 == 0;
        int field = (rng >> 8) & 1;
        float dipped = dip ? c_floor : 1.7f * c_floor;
        t.update(f, field == 0 ? dipped : 1.7f * c_floor,
                 field == 1 ? dipped : 1.7f * c_floor, -1.0f);
        REQUIRE_FALSE(t.isLocked());
    }
}

TEST_CASE("classification adapts to the overall level") {
    NullLogger log;
    NtscCadenceTracker t(log);
    for (int f = 2; f < 30; f++)
        t.update(f, f % 5 == 1 ? 4 * c_floor : 4 * c_motion,
                 f % 5 == 3 ? 4 * c_floor : 4 * c_motion, -1.0f);
    REQUIRE(t.isLocked());
}

TEST_CASE("the noise hint blocks false repeats from weak periodic motion") {
    NullLogger log;
    NtscCadenceTracker t(log);
    // d0 dips periodically to a level well above the true noise floor; with
    // the hint the floor cannot inflate up to it, so the dips read as motion
    for (int f = 2; f < 200; f++)
        t.update(f, f % 5 == 1 ? 0.02f : 0.05f, 0.05f, 0.01f);
    REQUIRE_FALSE(t.isLocked());
}

TEST_CASE("MeasureFieldDiffs measures per field and nulls the subcarrier") {
    std::vector<int16_t> cur(NTSC_TOTAL_WIDTH * NTSC_TOTAL_HEIGHT);
    std::vector<int16_t> prev(NTSC_TOTAL_WIDTH * NTSC_TOTAL_HEIGHT);
    auto h = [](float v) { return (int16_t)HalfFloatUtil::float_to_half(v); };

    SECTION("luma step in the second field only") {
        std::fill(cur.begin(), cur.end(), h(0.3f));
        std::fill(prev.begin(), prev.end(), h(0.3f));
        for (int row = 300; row < 400; row++)
            for (int x = 0; x < NTSC_TOTAL_WIDTH; x++)
                cur[row * NTSC_TOTAL_WIDTH + x] = h(0.4f);
        auto d = NtscCadenceTracker::MeasureFieldDiffs(cur.data(), prev.data());
        REQUIRE(d.d0 < 0.001f);
        REQUIRE(d.d1 > 0.02f);
    }

    SECTION("chroma that inverts between frames cancels in the box") {
        static constexpr float pat[4] = {0.2f, 0.0f, -0.2f, 0.0f};
        for (int row = 0; row < NTSC_TOTAL_HEIGHT; row++)
            for (int x = 0; x < NTSC_TOTAL_WIDTH; x++) {
                cur[row * NTSC_TOTAL_WIDTH + x] = h(0.3f + pat[x % 4]);
                prev[row * NTSC_TOTAL_WIDTH + x] = h(0.3f - pat[x % 4]);
            }
        auto d = NtscCadenceTracker::MeasureFieldDiffs(cur.data(), prev.data());
        REQUIRE(d.d0 < 0.002f);
        REQUIRE(d.d1 < 0.002f);
    }
}
