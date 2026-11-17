// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

// The MUSE disc code as the display and the position search read it: a frame
// address on CAV and CLV discs alike, the time of a CLV disc counted from it.

#include <catch2/catch_test_macros.hpp>
#include "muse/DiscCode.h"

namespace {
const int c_clv = 0x20, c_toc = 0x80;
}

TEST_CASE("A MUSE CLV disc shows its time beside the frame, and can be searched by either", "[DiscCode]") {
    const DiscCode code(c_clv, 7, 18001, 0); // ten minutes in
    REQUIRE(code.asStrings() == std::vector<std::string>{"MUSE 30 cm CLV", "Chapter 7 Time 0:10:00 Frame 18001"});
    REQUIRE(code.isClv() == true);
    REQUIRE(code.discTimeSeconds() == 600.0);
    REQUIRE(code.frameNumber() == 18001);
    REQUIRE(DiscCode(c_clv, 12, 1 + 30 * 3723 + 29, 0).asStrings()[1] == "Chapter 12 Time 1:02:03 Frame 111720");
}

TEST_CASE("A MUSE CAV disc shows its frame only", "[DiscCode]") {
    const DiscCode code(0, 3, 3485, 0);
    REQUIRE(code.asStrings() == std::vector<std::string>{"MUSE 30 cm CAV", "Chapter 3 Frame 3485"});
    REQUIRE(code.isClv() == false);
    REQUIRE(code.frameNumber() == 3485);
}

TEST_CASE("The frames before the programme and the lead-in have no time and no frame to search for", "[DiscCode]") {
    const DiscCode pre_roll(c_clv, 0, 131060, 0); // counting up to frame 0
    REQUIRE(pre_roll.asStrings()[1] == "Chapter 0 Frame 131060");
    REQUIRE(!pre_roll.discTimeSeconds());
    REQUIRE(!pre_roll.frameNumber());
    const DiscCode lead_in(c_clv | c_toc, 0, 500, 0);
    REQUIRE(lead_in.asStrings()[1] == "Chapter 0 Frame 500");
    REQUIRE(!lead_in.frameNumber());
}
