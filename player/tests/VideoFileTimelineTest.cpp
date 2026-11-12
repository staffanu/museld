// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <catch2/catch_all.hpp>
#include <optional>
#include "VideoFileTimeline.h"

namespace {
    // Feeds `count` frames of a disc playing steadily: file frame i of the
    // run shows disc frame first_disc_frame + i / frames_per_code
    void play(VideoFileTimeline &timeline, double fps, int64_t first_disc_frame, int count,
              std::optional<int> chapter, int frames_per_code = 1) {
        for (int i = 0; i < count; i++)
            timeline.addFrame((double)(first_disc_frame + i / frames_per_code) * frames_per_code / fps, chapter);
    }

    void blank(VideoFileTimeline &timeline, int count, std::optional<int> chapter = std::nullopt) {
        for (int i = 0; i < count; i++)
            timeline.addFrame(std::nullopt, chapter);
    }
}

TEST_CASE("timeline: a file from the middle of a side starts at the disc's time", "[timeline]") {
    VideoFileTimeline timeline(30, 1);
    play(timeline, 30.0, 18000, 14, 3);
    CHECK_FALSE(timeline.startDecided());
    play(timeline, 30.0, 18014, 1, 3);
    REQUIRE(timeline.startDecided());
    CHECK(timeline.startFrames() == 18000);
    CHECK(timeline.startTimecode() == "00:10:00:00");
    REQUIRE(timeline.chapters().size() == 1);
    CHECK(timeline.chapters()[0].number == 3);
    CHECK(timeline.chapters()[0].frame == 0);
}

TEST_CASE("timeline: a file that begins before the programme has a start below zero", "[timeline]") {
    // The capture this was built on: 13 frames of pre-roll without a time,
    // two frames of the programme, the player jumps back, pre-roll again,
    // and the programme runs from file frame 35
    VideoFileTimeline timeline(30, 1);
    blank(timeline, 13, 1);
    play(timeline, 30.0, 0, 2, 0);
    blank(timeline, 20, 1);
    play(timeline, 30.0, 0, 200, 0);
    REQUIRE(timeline.startDecided());
    CHECK(timeline.startFrames() == -35);
    CHECK(timeline.startTimecode() == "23:59:58:25");
    // The pre-roll's chapter number is no chapter of the file, and two
    // frames of the programme before the jump back are not a mark
    REQUIRE(timeline.chapters().size() == 1);
    CHECK(timeline.chapters()[0].number == 0);
    CHECK(timeline.chapters()[0].frame == 35);
}

TEST_CASE("timeline: a frame per field reads each code twice", "[timeline]") {
    for (int lead : {0, 1}) { // the file starts on the code's first or second field
        VideoFileTimeline timeline(60000, 1001);
        const double fps = 60000.0 / 1001.0;
        if (lead)
            timeline.addFrame(99.0 * 2 / fps, std::nullopt);
        play(timeline, fps, 100, 60, std::nullopt, 2);
        REQUIRE(timeline.startDecided());
        CHECK(timeline.startFrames() == 200 - lead);
        CHECK(timeline.timecodeFps() == 60);
    }
}

TEST_CASE("timeline: no steady code, no timecode", "[timeline]") {
    VideoFileTimeline timeline(30, 1);
    blank(timeline, 30 * 60 - 1);
    CHECK_FALSE(timeline.startDecided());
    blank(timeline, 1);
    CHECK(timeline.startDecided());
    CHECK_FALSE(timeline.startFrames());
    CHECK_FALSE(timeline.startTimecode());
    CHECK(timeline.chapters().empty());

    // A code that stands still (a still frame) or counts seconds only is
    // not a time that runs with the file
    VideoFileTimeline still(30, 1);
    for (int i = 0; i < 100; i++)
        still.addFrame(12.0, 1);
    CHECK_FALSE(still.startDecided());
    still.giveUp();
    CHECK(still.startDecided());
    CHECK_FALSE(still.startTimecode());
}

TEST_CASE("timeline: the file starts in the chapter read within its first half second", "[timeline]") {
    VideoFileTimeline timeline(30, 1);
    blank(timeline, 2); // no code on the first frames decoded
    play(timeline, 30.0, 502, 60, 7);
    REQUIRE(timeline.chapters().size() == 1);
    CHECK(timeline.chapters()[0].number == 7);
    CHECK(timeline.chapters()[0].frame == 0);
    CHECK(timeline.startFrames() == 500);
}

TEST_CASE("timeline: chapter marks", "[timeline]") {
    VideoFileTimeline timeline(30, 1);
    play(timeline, 30.0, 0, 100, 1);
    play(timeline, 30.0, 100, 1, 9);   // one misread code
    play(timeline, 30.0, 101, 99, 1);
    play(timeline, 30.0, 200, 100, 2);
    // frames whose code line failed carry no chapter and do not end a run
    play(timeline, 30.0, 300, 5, 3);
    play(timeline, 30.0, 305, 3, std::nullopt);
    play(timeline, 30.0, 308, 50, 3);
    REQUIRE(timeline.chapters().size() == 3);
    CHECK(timeline.chapters()[0].number == 1);
    CHECK(timeline.chapters()[0].frame == 0);
    CHECK(timeline.chapters()[1].number == 2);
    CHECK(timeline.chapters()[1].frame == 200);
    CHECK(timeline.chapters()[2].number == 3);
    CHECK(timeline.chapters()[2].frame == 300);

    // The next side written into the same file starts in the chapter the
    // first one ended in: it still gets its mark
    timeline.newInput();
    play(timeline, 30.0, 0, 50, 3);
    REQUIRE(timeline.chapters().size() == 4);
    CHECK(timeline.chapters()[3].number == 3);
    CHECK(timeline.chapters()[3].frame == 358);
    CHECK(timeline.startFrames() == 0); // the file's start stays the first side's
}
