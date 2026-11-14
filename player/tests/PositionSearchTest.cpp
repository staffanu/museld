// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <catch2/catch_all.hpp>
#include <cstdint>
#include <cmath>
#include <vector>
#include "PositionSearch.h"

namespace {
    constexpr double c_fs = 40e6;
    constexpr double c_fps = 30000.0 / 1001.0;

    // A synthetic capture: chapter starts in seconds of disc time, from a
    // capture that begins at `start_seconds` of the disc (chapter numbers
    // from `first_chapter`), lead-out after the last chapter
    struct Disc {
        std::vector<double> chapter_starts; // seconds, ascending; chapter i = first_chapter + i
        int first_chapter = 1;
        double capture_start = 0;           // disc seconds at input offset 0
        double length_seconds;              // of the capture
        std::optional<double> lead_out_at;  // disc seconds; nullopt = none in the capture
        // Chapter numbers only every other frame, as on a CLV disc whose
        // other fields carry the time code (before the inherit)
        bool sparse = false;
        bool cav = false;           // frame numbers instead of the time code
        bool whole_seconds = false; // a CLV time code without picture numbers: whole seconds only

        [[nodiscard]] int64_t sampleCount() const { return (int64_t)(length_seconds * c_fs); }

        [[nodiscard]] PositionSearch::Reading readingAt(int64_t offset, int frame_index) const {
            const double t = capture_start + (double)offset / c_fs;
            PositionSearch::Reading r{offset};
            if (lead_out_at && t >= *lead_out_at) {
                r.lead_out = true;
                return r;
            }
            int chapter = -1;
            for (size_t i = 0; i < chapter_starts.size(); i++)
                if (t >= chapter_starts[i])
                    chapter = first_chapter + (int)i;
            if (chapter >= 0 && !(sparse && frame_index == 0))
                r.chapter = chapter;
            // The disc's frames count from 1 at its start; a second of the
            // time code is 30 of them
            const auto frames = (int64_t)std::floor(t * c_fps);
            if (cav)
                r.frame = (int)frames + 1;
            else {
                r.time = whole_seconds ? (double)(frames / 30) : (double)frames / 30.0;
                r.time_resolution = whole_seconds ? 1.0 : 1.0 / 30;
            }
            return r;
        }
    };

    struct Outcome {
        PositionSearch::Action action;
        int probes;
        int frames;
    };

    // Runs a search to its end, simulating the reader: a probe at offset o
    // delivers whole frames from the first frame boundary after o plus some
    // block slack, one frame apart
    Outcome run(const Disc &disc, PositionSearch &search, double slack_seconds = 0.03) {
        int frames = 0;
        const int64_t frame = (int64_t)(c_fs / c_fps);
        for (int guard = 0; guard < 400; guard++) {
            const auto &a = search.action();
            if (a.kind == PositionSearch::Action::Kind::eDone || a.kind == PositionSearch::Action::Kind::eFailed)
                return {a, search.probes(), frames};
            REQUIRE(a.kind == PositionSearch::Action::Kind::eSeek);
            REQUIRE(a.offset >= 0);
            REQUIRE(a.offset < disc.sampleCount());
            int64_t landed = ((a.offset + (int64_t)(slack_seconds * c_fs)) / frame + 1) * frame;
            for (int i = 0; ; i++) {
                frames++;
                const auto &next = search.feed(disc.readingAt(landed, i));
                if (next.kind != PositionSearch::Action::Kind::eWait)
                    break;
                REQUIRE(i < 10);
                landed += frame;
            }
        }
        FAIL("search did not terminate");
        return {search.action(), search.probes(), frames};
    }

    PositionSearch::Params params(const Disc &disc) {
        return PositionSearch::Params{c_fs, disc.sampleCount()};
    }

    // Disc seconds of a found offset
    double discSeconds(const Disc &disc, int64_t offset) {
        return disc.capture_start + (double)offset / c_fs;
    }
}

TEST_CASE("next chapter is found just after its start", "[positionsearch]") {
    Disc disc{{0, 310, 611, 1500, 1501.5, 2400, 3300}, 1, 0, 3600};
    const int64_t origin = (int64_t)(100 * c_fs);
    PositionSearch search(params(disc), disc.readingAt(origin, 1), PositionSearch::Target::nextChapter());
    auto out = run(disc, search);
    REQUIRE(out.action.kind == PositionSearch::Action::Kind::eDone);
    CHECK(out.action.value == 2);
    const double found = discSeconds(disc, out.action.offset);
    CHECK(found >= 310);
    CHECK(found < 310 + 0.2);
    // Galloping from 100 s: 130, 190, 310+ ... then bisection of a <=120 s
    // bracket down to 0.15 s is ~10 steps
    CHECK(out.probes <= 16);
}

TEST_CASE("interpolation beats bisection when chapters are evenly spaced", "[positionsearch]") {
    Disc disc{{}, 1, 0, 3600};
    for (int i = 0; i < 40; i++)
        disc.chapter_starts.push_back(i * 90.0);
    // From chapter 1, to chapter 2: the first guess at +30 s doubles to 90
    const int64_t origin = (int64_t)(10 * c_fs);
    PositionSearch search(params(disc), disc.readingAt(origin, 1), PositionSearch::Target::nextChapter());
    auto out = run(disc, search);
    REQUIRE(out.action.kind == PositionSearch::Action::Kind::eDone);
    CHECK(out.action.value == 2);
    CHECK(discSeconds(disc, out.action.offset) >= 90);
    CHECK(discSeconds(disc, out.action.offset) < 90.2);
    CHECK(out.probes <= 14);
}

TEST_CASE("previous chapter goes to the start of the current one", "[positionsearch]") {
    Disc disc{{0, 310, 611, 1500}, 1, 0, 3600};
    const int64_t origin = (int64_t)(700 * c_fs); // 89 s into chapter 3
    PositionSearch search(params(disc), disc.readingAt(origin, 1), PositionSearch::Target::previousChapter());
    auto out = run(disc, search);
    REQUIRE(out.action.kind == PositionSearch::Action::Kind::eDone);
    CHECK(out.action.value == 3);
    CHECK(discSeconds(disc, out.action.offset) >= 611);
    CHECK(discSeconds(disc, out.action.offset) < 611.2);
}

TEST_CASE("previous chapter from just inside a chapter goes one further back", "[positionsearch]") {
    Disc disc{{0, 310, 611, 1500}, 1, 0, 3600};
    const int64_t origin = (int64_t)(612 * c_fs); // 1 s into chapter 3
    PositionSearch search(params(disc), disc.readingAt(origin, 1), PositionSearch::Target::previousChapter());
    auto out = run(disc, search);
    REQUIRE(out.action.kind == PositionSearch::Action::Kind::eDone);
    CHECK(out.action.value == 2);
    CHECK(discSeconds(disc, out.action.offset) >= 310);
    CHECK(discSeconds(disc, out.action.offset) < 310.2);
}

TEST_CASE("a chapter that started before the capture lands at the start", "[positionsearch]") {
    Disc disc{{0, 310, 611}, 1, 400, 1000}; // capture starts 90 s into chapter 2
    const int64_t origin = (int64_t)(50 * c_fs);
    PositionSearch search(params(disc), disc.readingAt(origin, 1), PositionSearch::Target::previousChapter());
    auto out = run(disc, search);
    REQUIRE(out.action.kind == PositionSearch::Action::Kind::eDone);
    CHECK(out.action.offset == 0);
    CHECK(out.action.value == 2);
}

TEST_CASE("no next chapter on the capture fails instead of reading past the end", "[positionsearch]") {
    Disc disc{{0, 310}, 1, 0, 900};
    const int64_t origin = (int64_t)(400 * c_fs);
    PositionSearch search(params(disc), disc.readingAt(origin, 1), PositionSearch::Target::nextChapter());
    auto out = run(disc, search);
    REQUIRE(out.action.kind == PositionSearch::Action::Kind::eFailed);
    CHECK(out.action.reason == "NO MORE CHAPTERS");
}

TEST_CASE("a capture whose tail has no signal ends a forward search", "[positionsearch]") {
    // Nothing decodes in the last three seconds: the probe aimed at the
    // input's end reads no frame, and the search looks before it
    Disc disc{{0, 310}, 1, 0, 900};
    const int64_t origin = (int64_t)(400 * c_fs);
    const int64_t signal_end = (int64_t)(897 * c_fs);
    const int64_t frame = (int64_t)(c_fs / c_fps);
    for (bool chapter_near_end : {false, true}) {
        if (chapter_near_end)
            disc.chapter_starts.push_back(880);
        PositionSearch search(params(disc), disc.readingAt(origin, 1), PositionSearch::Target::nextChapter());
        int ended = 0;
        for (int guard = 0; guard < 100; guard++) {
            const auto &a = search.action();
            if (a.kind != PositionSearch::Action::Kind::eSeek)
                break;
            const int64_t landed = (a.offset / frame + 1) * frame;
            if (landed >= signal_end) {
                search.probeReachedEnd();
                ended++;
            } else {
                search.feed(disc.readingAt(landed, 1));
            }
        }
        CHECK(ended >= 1);
        if (chapter_near_end) {
            REQUIRE(search.action().kind == PositionSearch::Action::Kind::eDone);
            CHECK(search.action().value == 3);
            CHECK(std::abs(discSeconds(disc, search.action().offset) - 880) < 0.3);
        } else {
            REQUIRE(search.action().kind == PositionSearch::Action::Kind::eFailed);
            CHECK(search.action().reason == "NO MORE CHAPTERS");
        }
    }
}

TEST_CASE("the lead-out ends a forward search", "[positionsearch]") {
    Disc disc{{0, 310}, 1, 0, 1800, 900.0};
    const int64_t origin = (int64_t)(400 * c_fs);
    PositionSearch search(params(disc), disc.readingAt(origin, 1), PositionSearch::Target::nextChapter());
    auto out = run(disc, search);
    REQUIRE(out.action.kind == PositionSearch::Action::Kind::eFailed);
    CHECK(out.action.reason == "NO MORE CHAPTERS");
}

TEST_CASE("sparse chapter codes only cost a frame per probe", "[positionsearch]") {
    Disc disc{{0, 310, 611, 1500}, 1, 0, 3600};
    disc.sparse = true;
    const int64_t origin = (int64_t)(100 * c_fs);
    PositionSearch search(params(disc), disc.readingAt(origin, 1), PositionSearch::Target::nextChapter());
    auto out = run(disc, search);
    REQUIRE(out.action.kind == PositionSearch::Action::Kind::eDone);
    CHECK(out.action.value == 2);
    CHECK(out.frames <= 2 * out.probes);
}

TEST_CASE("a position without a chapter code cannot start a search", "[positionsearch]") {
    Disc disc{{0, 310}, 1, 0, 900};
    PositionSearch search(params(disc), PositionSearch::Reading{0}, PositionSearch::Target::nextChapter());
    CHECK(search.action().kind == PositionSearch::Action::Kind::eFailed);
}

TEST_CASE("a large landing slack still converges", "[positionsearch]") {
    Disc disc{{0, 310, 611, 1500}, 1, 0, 3600};
    const int64_t origin = (int64_t)(100 * c_fs);
    PositionSearch search(params(disc), disc.readingAt(origin, 1), PositionSearch::Target::nextChapter());
    auto out = run(disc, search, 0.4);
    REQUIRE(out.action.kind == PositionSearch::Action::Kind::eDone);
    CHECK(out.action.value == 2);
    CHECK(discSeconds(disc, out.action.offset) >= 310);
    CHECK(discSeconds(disc, out.action.offset) < 311);
}

// Disc seconds at which the time code reads `code_seconds`
static double discSecondsOfCode(double code_seconds) { return code_seconds * 30.0 / c_fps; }

TEST_CASE("a time search lands at the time code in a few probes", "[positionsearch]") {
    Disc disc{{0, 310, 611, 1500}, 1, 0, 3600};
    for (bool whole_seconds : {false, true}) {
        disc.whole_seconds = whole_seconds;
        const int64_t origin = (int64_t)(100 * c_fs);
        // 0:51:31 of the time code, 3094 s into the disc at 29.97 frames/s
        PositionSearch search(params(disc), disc.readingAt(origin, 1), PositionSearch::Target::time(3091));
        auto out = run(disc, search);
        REQUIRE(out.action.kind == PositionSearch::Action::Kind::eDone);
        CHECK(out.action.value == 3091);
        const auto there = disc.readingAt(out.action.offset, 1);
        REQUIRE(there.time.has_value());
        CHECK(*there.time >= 3091);
        const double found = discSeconds(disc, out.action.offset);
        CHECK(found >= discSecondsOfCode(3091));
        CHECK(found < discSecondsOfCode(3091) + 0.2);
        CHECK(out.probes <= (whole_seconds ? 6 : 4));
    }
}

TEST_CASE("a time search goes backwards too", "[positionsearch]") {
    Disc disc{{0, 310, 611, 1500}, 1, 0, 3600};
    const int64_t origin = (int64_t)(3000 * c_fs);
    PositionSearch search(params(disc), disc.readingAt(origin, 1), PositionSearch::Target::time(100));
    auto out = run(disc, search);
    REQUIRE(out.action.kind == PositionSearch::Action::Kind::eDone);
    const double found = discSeconds(disc, out.action.offset);
    CHECK(found >= discSecondsOfCode(100));
    CHECK(found < discSecondsOfCode(100) + 0.2);
    CHECK(out.probes <= 4);
}

TEST_CASE("a frame search lands on the frame", "[positionsearch]") {
    Disc disc{{0, 310, 611}, 1, 0, 1800};
    disc.cav = true;
    for (int target : {23456, 1000}) {
        const int64_t origin = (int64_t)(100 * c_fs); // frame 2998
        PositionSearch search(params(disc), disc.readingAt(origin, 1), PositionSearch::Target::frame(target));
        auto out = run(disc, search);
        REQUIRE(out.action.kind == PositionSearch::Action::Kind::eDone);
        CHECK(out.action.value == target);
        const auto there = disc.readingAt(out.action.offset, 1);
        REQUIRE(there.frame.has_value());
        CHECK(*there.frame == target);
        CHECK(out.probes <= 5);
        // The player seeks to seek_offset and is handed the first whole frame
        // past it, which must be that frame
        const int64_t frame = (int64_t)(c_fs / c_fps);
        const int64_t landed = ((out.action.seek_offset + (int64_t)(0.03 * c_fs)) / frame + 1) * frame;
        CHECK(landed == out.action.offset);
    }
}

TEST_CASE("a frame search for the frame on screen is done at once", "[positionsearch]") {
    Disc disc{{0, 310, 611}, 1, 0, 1800};
    disc.cav = true;
    const int64_t origin = (int64_t)(100 * c_fs);
    const auto here = disc.readingAt(origin, 1);
    PositionSearch search(params(disc), here, PositionSearch::Target::frame(*here.frame));
    REQUIRE(search.action().kind == PositionSearch::Action::Kind::eDone);
    CHECK(search.action().offset == origin);
    CHECK(search.probes() == 0);
}

TEST_CASE("a frame past the end of the capture fails", "[positionsearch]") {
    Disc disc{{0, 310}, 1, 0, 900, 800.0};
    disc.cav = true;
    const int64_t origin = (int64_t)(100 * c_fs);
    PositionSearch search(params(disc), disc.readingAt(origin, 1), PositionSearch::Target::frame(50000));
    auto out = run(disc, search);
    REQUIRE(out.action.kind == PositionSearch::Action::Kind::eFailed);
    CHECK(out.action.reason == "NOT FOUND");
}

TEST_CASE("a time before the capture began lands at its start", "[positionsearch]") {
    Disc disc{{0, 310, 611}, 1, 400, 1000};
    const int64_t origin = (int64_t)(50 * c_fs);
    PositionSearch search(params(disc), disc.readingAt(origin, 1), PositionSearch::Target::time(100));
    auto out = run(disc, search);
    REQUIRE(out.action.kind == PositionSearch::Action::Kind::eDone);
    CHECK(out.action.offset == 0);
}

TEST_CASE("a target in a coordinate the disc lacks fails at once", "[positionsearch]") {
    Disc disc{{0, 310}, 1, 0, 900};
    disc.cav = true;
    PositionSearch by_time(params(disc), disc.readingAt(0, 1), PositionSearch::Target::time(100));
    REQUIRE(by_time.action().kind == PositionSearch::Action::Kind::eFailed);
    CHECK(by_time.action().reason == "NO TIME CODE");
    disc.cav = false;
    PositionSearch by_frame(params(disc), disc.readingAt(0, 1), PositionSearch::Target::frame(100));
    REQUIRE(by_frame.action().kind == PositionSearch::Action::Kind::eFailed);
    CHECK(by_frame.action().reason == "NO FRAME NUMBER");
}

TEST_CASE("positions are named as the OSD shows them", "[positionsearch]") {
    using Kind = PositionSearch::Target::Kind;
    CHECK(PositionSearch::describe(Kind::eChapter, 12) == "CHAPTER 12");
    CHECK(PositionSearch::describe(Kind::eNextChapter, 3) == "CHAPTER 3");
    CHECK(PositionSearch::describe(Kind::eTime, 3091) == "0:51:31");
    CHECK(PositionSearch::describe(Kind::eTime, 3600) == "1:00:00");
    CHECK(PositionSearch::describe(Kind::eFrame, 23456) == "FRAME 23456");
}
