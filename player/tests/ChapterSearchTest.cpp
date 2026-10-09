// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <catch2/catch_all.hpp>
#include <cstdint>
#include <vector>
#include "ChapterSearch.h"

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

        [[nodiscard]] int64_t sampleCount() const { return (int64_t)(length_seconds * c_fs); }

        [[nodiscard]] ChapterSearch::Reading readingAt(int64_t offset, int frame_index) const {
            const double t = capture_start + (double)offset / c_fs;
            ChapterSearch::Reading r{offset};
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
            return r;
        }
    };

    struct Outcome {
        ChapterSearch::Action action;
        int probes;
        int frames;
    };

    // Runs a search to its end, simulating the reader: a probe at offset o
    // delivers whole frames from the first frame boundary after o plus some
    // block slack, one frame apart
    Outcome run(const Disc &disc, ChapterSearch &search, double slack_seconds = 0.03) {
        int frames = 0;
        const int64_t frame = (int64_t)(c_fs / c_fps);
        for (int guard = 0; guard < 400; guard++) {
            const auto &a = search.action();
            if (a.kind == ChapterSearch::Action::Kind::eDone || a.kind == ChapterSearch::Action::Kind::eFailed)
                return {a, search.probes(), frames};
            REQUIRE(a.kind == ChapterSearch::Action::Kind::eSeek);
            REQUIRE(a.offset >= 0);
            REQUIRE(a.offset < disc.sampleCount());
            int64_t landed = ((a.offset + (int64_t)(slack_seconds * c_fs)) / frame + 1) * frame;
            for (int i = 0; ; i++) {
                frames++;
                const auto &next = search.feed(disc.readingAt(landed, i));
                if (next.kind != ChapterSearch::Action::Kind::eWait)
                    break;
                REQUIRE(i < 10);
                landed += frame;
            }
        }
        FAIL("search did not terminate");
        return {search.action(), search.probes(), frames};
    }

    ChapterSearch::Params params(const Disc &disc) {
        return ChapterSearch::Params{c_fs, disc.sampleCount()};
    }

    // Disc seconds of a found offset
    double discSeconds(const Disc &disc, int64_t offset) {
        return disc.capture_start + (double)offset / c_fs;
    }
}

TEST_CASE("next chapter is found just after its start", "[chaptersearch]") {
    Disc disc{{0, 310, 611, 1500, 1501.5, 2400, 3300}, 1, 0, 3600};
    const int64_t origin = (int64_t)(100 * c_fs);
    ChapterSearch search(params(disc), disc.readingAt(origin, 1), ChapterSearch::Direction::eNext);
    auto out = run(disc, search);
    REQUIRE(out.action.kind == ChapterSearch::Action::Kind::eDone);
    CHECK(out.action.chapter == 2);
    const double found = discSeconds(disc, out.action.offset);
    CHECK(found >= 310);
    CHECK(found < 310 + 0.2);
    // Galloping from 100 s: 130, 190, 310+ ... then bisection of a <=120 s
    // bracket down to 0.15 s is ~10 steps
    CHECK(out.probes <= 16);
}

TEST_CASE("interpolation beats bisection when chapters are evenly spaced", "[chaptersearch]") {
    Disc disc{{}, 1, 0, 3600};
    for (int i = 0; i < 40; i++)
        disc.chapter_starts.push_back(i * 90.0);
    // From chapter 1, to chapter 2: the first guess at +30 s doubles to 90
    const int64_t origin = (int64_t)(10 * c_fs);
    ChapterSearch search(params(disc), disc.readingAt(origin, 1), ChapterSearch::Direction::eNext);
    auto out = run(disc, search);
    REQUIRE(out.action.kind == ChapterSearch::Action::Kind::eDone);
    CHECK(out.action.chapter == 2);
    CHECK(discSeconds(disc, out.action.offset) >= 90);
    CHECK(discSeconds(disc, out.action.offset) < 90.2);
    CHECK(out.probes <= 14);
}

TEST_CASE("previous chapter goes to the start of the current one", "[chaptersearch]") {
    Disc disc{{0, 310, 611, 1500}, 1, 0, 3600};
    const int64_t origin = (int64_t)(700 * c_fs); // 89 s into chapter 3
    ChapterSearch search(params(disc), disc.readingAt(origin, 1), ChapterSearch::Direction::ePrevious);
    auto out = run(disc, search);
    REQUIRE(out.action.kind == ChapterSearch::Action::Kind::eDone);
    CHECK(out.action.chapter == 3);
    CHECK(discSeconds(disc, out.action.offset) >= 611);
    CHECK(discSeconds(disc, out.action.offset) < 611.2);
}

TEST_CASE("previous chapter from just inside a chapter goes one further back", "[chaptersearch]") {
    Disc disc{{0, 310, 611, 1500}, 1, 0, 3600};
    const int64_t origin = (int64_t)(612 * c_fs); // 1 s into chapter 3
    ChapterSearch search(params(disc), disc.readingAt(origin, 1), ChapterSearch::Direction::ePrevious);
    auto out = run(disc, search);
    REQUIRE(out.action.kind == ChapterSearch::Action::Kind::eDone);
    CHECK(out.action.chapter == 2);
    CHECK(discSeconds(disc, out.action.offset) >= 310);
    CHECK(discSeconds(disc, out.action.offset) < 310.2);
}

TEST_CASE("a chapter that started before the capture lands at the start", "[chaptersearch]") {
    Disc disc{{0, 310, 611}, 1, 400, 1000}; // capture starts 90 s into chapter 2
    const int64_t origin = (int64_t)(50 * c_fs);
    ChapterSearch search(params(disc), disc.readingAt(origin, 1), ChapterSearch::Direction::ePrevious);
    auto out = run(disc, search);
    REQUIRE(out.action.kind == ChapterSearch::Action::Kind::eDone);
    CHECK(out.action.offset == 0);
    CHECK(out.action.chapter == 2);
}

TEST_CASE("no next chapter on the capture fails instead of reading past the end", "[chaptersearch]") {
    Disc disc{{0, 310}, 1, 0, 900};
    const int64_t origin = (int64_t)(400 * c_fs);
    ChapterSearch search(params(disc), disc.readingAt(origin, 1), ChapterSearch::Direction::eNext);
    auto out = run(disc, search);
    REQUIRE(out.action.kind == ChapterSearch::Action::Kind::eFailed);
    CHECK(out.action.reason == "NO MORE CHAPTERS");
}

TEST_CASE("the lead-out ends a forward search", "[chaptersearch]") {
    Disc disc{{0, 310}, 1, 0, 1800, 900.0};
    const int64_t origin = (int64_t)(400 * c_fs);
    ChapterSearch search(params(disc), disc.readingAt(origin, 1), ChapterSearch::Direction::eNext);
    auto out = run(disc, search);
    REQUIRE(out.action.kind == ChapterSearch::Action::Kind::eFailed);
    CHECK(out.action.reason == "NO MORE CHAPTERS");
}

TEST_CASE("sparse chapter codes only cost a frame per probe", "[chaptersearch]") {
    Disc disc{{0, 310, 611, 1500}, 1, 0, 3600};
    disc.sparse = true;
    const int64_t origin = (int64_t)(100 * c_fs);
    ChapterSearch search(params(disc), disc.readingAt(origin, 1), ChapterSearch::Direction::eNext);
    auto out = run(disc, search);
    REQUIRE(out.action.kind == ChapterSearch::Action::Kind::eDone);
    CHECK(out.action.chapter == 2);
    CHECK(out.frames <= 2 * out.probes);
}

TEST_CASE("a position without a chapter code cannot start a search", "[chaptersearch]") {
    Disc disc{{0, 310}, 1, 0, 900};
    ChapterSearch search(params(disc), ChapterSearch::Reading{0}, ChapterSearch::Direction::eNext);
    CHECK(search.action().kind == ChapterSearch::Action::Kind::eFailed);
}

TEST_CASE("a large landing slack still converges", "[chaptersearch]") {
    Disc disc{{0, 310, 611, 1500}, 1, 0, 3600};
    const int64_t origin = (int64_t)(100 * c_fs);
    ChapterSearch search(params(disc), disc.readingAt(origin, 1), ChapterSearch::Direction::eNext);
    auto out = run(disc, search, 0.4);
    REQUIRE(out.action.kind == ChapterSearch::Action::Kind::eDone);
    CHECK(out.action.chapter == 2);
    CHECK(discSeconds(disc, out.action.offset) >= 310);
    CHECK(discSeconds(disc, out.action.offset) < 311);
}
