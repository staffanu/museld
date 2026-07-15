// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <catch2/catch_all.hpp>

#include "subtitles/SrtParser.h"


TEST_CASE("Basic SRT parsing") {
    std::string text =
        "1\n"
        "00:00:01,000 --> 00:00:03,500\n"
        "Hello world\n"
        "\n"
        "2\n"
        "00:00:04,000 --> 00:00:05,000\n"
        "Second line\n"
        "Across two rows\n"
        "\n";
    auto entries = parseSrtText(text);
    REQUIRE(entries.size() == 2);
    REQUIRE(entries[0].start_seconds == Catch::Approx(1.0));
    REQUIRE(entries[0].end_seconds == Catch::Approx(3.5));
    REQUIRE(entries[0].lines == std::vector<std::string>{"Hello world"});
    REQUIRE(entries[1].lines.size() == 2);
    REQUIRE(entries[1].lines[1] == "Across two rows");
}

TEST_CASE("Accepts CRLF and BOM") {
    std::string text =
        "\xEF\xBB\xBF"
        "1\r\n"
        "00:00:00,500 --> 00:00:01,500\r\n"
        "Hi\r\n"
        "\r\n";
    auto entries = parseSrtText(text);
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].lines == std::vector<std::string>{"Hi"});
}

TEST_CASE("Strips HTML-like tags and decodes entities") {
    std::string text =
        "1\n"
        "00:00:01,000 --> 00:00:02,000\n"
        "<i>Tom &amp; Jerry</i>\n"
        "&#65;&#x42;\n"
        "\n";
    auto entries = parseSrtText(text);
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].lines[0] == "Tom & Jerry");
    REQUIRE(entries[0].lines[1] == "AB");
}

TEST_CASE("Accepts '.' as millisecond separator") {
    std::string text =
        "1\n"
        "00:00:01.250 --> 00:00:02.750\n"
        "ok\n";
    auto entries = parseSrtText(text);
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].start_seconds == Catch::Approx(1.25));
    REQUIRE(entries[0].end_seconds == Catch::Approx(2.75));
}

TEST_CASE("Sorts out-of-order entries by start time") {
    std::string text =
        "2\n"
        "00:00:10,000 --> 00:00:11,000\n"
        "second\n"
        "\n"
        "1\n"
        "00:00:01,000 --> 00:00:02,000\n"
        "first\n"
        "\n";
    auto entries = parseSrtText(text);
    REQUIRE(entries.size() == 2);
    REQUIRE(entries[0].lines[0] == "first");
    REQUIRE(entries[1].lines[0] == "second");
}

TEST_CASE("Ignores positioning extension after second timestamp") {
    std::string text =
        "1\n"
        "00:00:01,000 --> 00:00:02,000 X1:10 X2:20 Y1:30 Y2:40\n"
        "ok\n";
    auto entries = parseSrtText(text);
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].end_seconds == Catch::Approx(2.0));
}
