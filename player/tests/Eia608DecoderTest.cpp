// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <string>
#include <vector>

#include "subtitles/Eia608Decoder.h"
#include "logging/StreamLogger.h"

namespace {

Logger &testLog() {
    static StreamLogger log(StreamLogger::c_log_error, std::cerr, false);
    return log;
}

uint8_t withParity(uint8_t b) {
    return (std::popcount(b) & 1) ? b : (uint8_t)(b | 0x80);
}

// Feeds one pair; control pairs are doubled like a real transmission.
std::optional<std::vector<std::string>> feed(Eia608Decoder &d, uint8_t b1, uint8_t b2) {
    auto r = d.process(withParity(b1), withParity(b2));
    if (b1 >= 0x10 && b1 <= 0x1f) {
        auto r2 = d.process(withParity(b1), withParity(b2));
        REQUIRE(!r2.has_value()); // the doubled retransmission must be inert
    }
    return r;
}

std::optional<std::vector<std::string>> feedText(Eia608Decoder &d, const std::string &text) {
    std::optional<std::vector<std::string>> last;
    for (size_t i = 0; i < text.size(); i += 2) {
        uint8_t b2 = i + 1 < text.size() ? text[i + 1] : 0;
        if (auto r = d.process(withParity((uint8_t)text[i]), withParity(b2)))
            last = r;
    }
    return last;
}

constexpr uint8_t RCL = 0x20, EDM = 0x2c, ENM = 0x2e, EOC = 0x2f, CR = 0x2d;
constexpr uint8_t RU2 = 0x25;

} // namespace

TEST_CASE("pop-on caption appears on EOC and clears on EDM", "[eia608]") {
    Eia608Decoder d(testLog());

    REQUIRE(!feed(d, 0x14, RCL));
    REQUIRE(!feed(d, 0x14, ENM));
    REQUIRE(!feed(d, 0x11, 0x50)); // PAC row 2, column 0
    REQUIRE(!feedText(d, "HELLO"));

    auto shown = feed(d, 0x14, EOC);
    REQUIRE(shown.has_value());
    REQUIRE(*shown == std::vector<std::string>{"HELLO"});

    auto cleared = feed(d, 0x14, EDM);
    REQUIRE(cleared.has_value());
    REQUIRE(cleared->empty());
}

TEST_CASE("two-row pop-on caption keeps row order", "[eia608]") {
    Eia608Decoder d(testLog());

    feed(d, 0x14, RCL);
    feed(d, 0x14, ENM);
    feed(d, 0x13, 0x40); // PAC row 12
    feedText(d, "FIRST LINE");
    feed(d, 0x13, 0x60); // PAC row 13
    feedText(d, "SECOND LINE");
    auto shown = feed(d, 0x14, EOC);
    REQUIRE(shown.has_value());
    REQUIRE(*shown == std::vector<std::string>{"FIRST LINE", "SECOND LINE"});
}

TEST_CASE("pop-on memories flip without erasing", "[eia608]") {
    Eia608Decoder d(testLog());

    feed(d, 0x14, RCL);
    feed(d, 0x13, 0x40);
    feedText(d, "ONE");
    REQUIRE(*feed(d, 0x14, EOC) == std::vector<std::string>{"ONE"});

    // Load the second caption into the flipped-out memory and show it
    feed(d, 0x14, RCL);
    feed(d, 0x14, ENM);
    feed(d, 0x13, 0x40);
    feedText(d, "TWO");
    REQUIRE(*feed(d, 0x14, EOC) == std::vector<std::string>{"TWO"});
}

TEST_CASE("roll-up types onto the screen and scrolls on CR", "[eia608]") {
    Eia608Decoder d(testLog());

    REQUIRE(!feed(d, 0x14, RU2));
    feedText(d, "FIRST");
    // A null pair pauses the typing, which flushes the pending text
    auto shown = d.process(0x80, 0x80);
    REQUIRE(shown.has_value());
    REQUIRE(*shown == std::vector<std::string>{"FIRST"});

    // CR moves FIRST up a row; the emitted lines are unchanged, so no update
    REQUIRE(!feed(d, 0x14, CR));
    feedText(d, "SECOND");
    auto both = d.process(0x80, 0x80);
    REQUIRE(both.has_value());
    REQUIRE(*both == std::vector<std::string>{"FIRST", "SECOND"});
}

TEST_CASE("special and extended characters", "[eia608]") {
    Eia608Decoder d(testLog());

    feed(d, 0x14, RCL);
    feed(d, 0x14, ENM);
    feed(d, 0x13, 0x40);
    feedText(d, "A");
    REQUIRE(!feed(d, 0x11, 0x37)); // music note
    // Extended characters replace the preceding basic-set fallback
    feedText(d, "e");
    REQUIRE(!feed(d, 0x12, 0x36)); // ë
    auto shown = feed(d, 0x14, EOC);
    REQUIRE(shown.has_value());
    REQUIRE((*shown)[0] == "A♪ë");
}

TEST_CASE("backspace and delete to end of row", "[eia608]") {
    Eia608Decoder d(testLog());

    feed(d, 0x14, RCL);
    feed(d, 0x14, ENM);
    feed(d, 0x13, 0x40);
    feedText(d, "ABCD");
    feed(d, 0x14, 0x21); // BS: removes D
    REQUIRE(*feed(d, 0x14, EOC) == std::vector<std::string>{"ABC"});
}

TEST_CASE("parity errors and channel 2 data are ignored", "[eia608]") {
    Eia608Decoder d(testLog());

    feed(d, 0x14, RCL);
    feed(d, 0x14, ENM);
    feed(d, 0x13, 0x40);
    // Corrupt pair: even parity on the first byte
    REQUIRE(!d.process(0x14, withParity(0x20)));
    feedText(d, "OK");
    // Channel 2 control selects channel 2; its text must not land in CC1
    d.process(withParity(0x1c), withParity(RCL));
    d.process(withParity('X'), withParity('X'));
    // Back to channel 1
    d.process(withParity(0x14), withParity(RCL));
    REQUIRE(*feed(d, 0x14, EOC) == std::vector<std::string>{"OK"});
}

TEST_CASE("basic character substitutions", "[eia608]") {
    Eia608Decoder d(testLog());

    feed(d, 0x14, RCL);
    feed(d, 0x14, ENM);
    feed(d, 0x13, 0x40);
    d.process(withParity(0x2a), withParity(0x7e)); // á ñ
    auto shown = feed(d, 0x14, EOC);
    REQUIRE(shown.has_value());
    REQUIRE((*shown)[0] == "áñ");
}
