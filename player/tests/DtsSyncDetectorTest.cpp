// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <catch2/catch_all.hpp>
#include <vector>
#include "sdtv/DtsSyncDetector.h"

namespace {
    TwoChannelSampleWithErasureFlags sample(int16_t l, int16_t r, bool l_erased = false, bool r_erased = false) {
        TwoChannelSampleWithErasureFlags s{};
        s.samples[0] = l;
        s.samples[1] = r;
        s.erased[0] = l_erased;
        s.erased[1] = r_erased;
        return s;
    }

    // Feed n reads with the given sync count each; returns the last state
    bool feed(DtsSyncDetector &d, int reads, int syncs) {
        bool state = d.active();
        for (int i = 0; i < reads; i++)
            state = d.update(syncs);
        return state;
    }
}

TEST_CASE("counts the DTS sync word in its four framings, across the channel boundary") {
    std::vector<TwoChannelSampleWithErasureFlags> s = {
            sample(0x7ffe, (int16_t)0x8001),          // 16-bit big endian, within a sample
            sample(100, 0x1fff), sample((int16_t)0xe800, 200), // 14-bit, R of one sample then L of the next
            sample((int16_t)0xfe7f, 0x0180),         // 16-bit little endian
            sample((int16_t)0xff1f, 0x00e8),         // 14-bit little endian
            sample(0x7ffe, 0x1234), sample((int16_t)0x8001, 0), // not a pair: 7ffe 1234 8001
    };
    CHECK(DtsSyncDetector::countSyncWords(s) == 4);
    CHECK(DtsSyncDetector::countSyncWords({}) == 0);
}

TEST_CASE("erased samples are not counted") {
    CHECK(DtsSyncDetector::countSyncWords({sample(0x7ffe, (int16_t)0x8001, true, false)}) == 0);
    CHECK(DtsSyncDetector::countSyncWords({sample(0x7ffe, (int16_t)0x8001, false, true)}) == 0);
    CHECK(DtsSyncDetector::countSyncWords({sample(0x7ffe, (int16_t)0x8001)}) == 1);
}

TEST_CASE("stray sync patterns in PCM do not switch the track to DTS") {
    DtsSyncDetector d;
    // The WOW disc: a hit about once a minute, sometimes two within a few seconds
    for (int minute = 0; minute < 30; minute++) {
        CHECK_FALSE(feed(d, 1700, 0));
        CHECK_FALSE(d.update(1));
        CHECK_FALSE(feed(d, 50, 0));
        CHECK_FALSE(d.update(2)); // the old detector flipped here: two hits within its hold-off
    }
    // Even a short burst below the density of the sparsest DTS stream stays PCM
    CHECK_FALSE(feed(d, 200, 0));
    for (int i = 0; i < 7; i++)
        CHECK_FALSE(d.update(1));
}

TEST_CASE("a DTS stream is detected within a second, at any DTS frame size") {
    SECTION("512-sample frames: about three sync words per read") {
        DtsSyncDetector d;
        int reads = 0;
        while (!d.update(3) && reads < 100)
            reads++;
        CHECK(reads <= 3);
    }
    SECTION("4096-sample frames: a sync word every third read") {
        DtsSyncDetector d;
        int reads = 0;
        bool active = false;
        while (!active && reads < 100) {
            active = d.update(reads % 3 == 0 ? 1 : 0);
            reads++;
        }
        CHECK(active);
        CHECK(reads <= DtsSyncDetector::c_window_reads);
    }
}

TEST_CASE("DTS holds through a dropout and releases after four seconds without sync words") {
    DtsSyncDetector d;
    REQUIRE(feed(d, 10, 3));
    CHECK(feed(d, DtsSyncDetector::c_release_reads - 1, 0)); // a long dropout: still DTS
    CHECK(d.update(3));                                       // the stream is back
    CHECK(feed(d, DtsSyncDetector::c_release_reads - 1, 0));
    CHECK_FALSE(d.update(0));                                 // gone for good: PCM again
    // and it needs the full density to come back, not a single hit
    CHECK_FALSE(d.update(1));
    CHECK(feed(d, 10, 3));
}
