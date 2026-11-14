// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <catch2/catch_all.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>
#include "DiscInfo.h"
#include "musevk/HalfFloatUtil.h"
#include "signature/DiscSignature.h"

using namespace disc_signature;

namespace {
    struct TempFile {
        std::string path = (std::filesystem::temp_directory_path() / "museld-signature-test.ldsig").string();
        ~TempFile() { std::remove(path.c_str()); }
    };

    struct Code : DiscInfo {
        std::optional<double> time;
        std::optional<int> chap;
        std::optional<bool> clv;
        double resolution = 1.0;
        bool lead_in = false;
        std::vector<std::string> asStrings() const override { return {}; }
        std::optional<double> discTimeSeconds() const override { return time; }
        double discTimeResolutionSeconds() const override { return resolution; }
        std::optional<int> chapter() const override { return chap; }
        std::optional<bool> isClv() const override { return clv; }
        bool isLeadIn() const override { return lead_in; }
    };
}

TEST_CASE("signature: block means cover the inner picture of each field", "[signature]") {
    // 100 samples per row; the fields' pictures are 40 rows of 80 columns
    // from column 10, at rows 5 and 55.  Margins: 2 rows, 8 columns, so
    // blocks of 3 rows by 4 columns from row 7 / 57, column 18.
    const FieldGeometry g{100, {5, 55}, 40, 10, 80};
    std::vector<uint16_t> buffer(100 * 100, HalfFloatUtil::float_to_half(9.0f)); // outside the grid: must not count
    for (int field = 0; field < 2; field++)
        for (int r = 0; r < 36; r++)
            for (int c = 0; c < 64; c++)
                buffer[(g.first_row[field] + 2 + r) * 100 + 18 + c] =
                        HalfFloatUtil::float_to_half(field + (r / 3) * 0.0625f + (c / 4) * 0.25f);
    std::array<float, c_block_count> blocks;
    computeBlocks(buffer.data(), g, blocks);
    for (int field = 0; field < 2; field++)
        for (int r = 0; r < c_rows; r++)
            for (int c = 0; c < c_columns; c++)
                CHECK(blocks[(field * c_rows + r) * c_columns + c] == Catch::Approx(field + r * 0.0625 + c * 0.25));
}

TEST_CASE("signature: the disc code becomes the disc's frame count", "[signature]") {
    Frame frame;
    Code code;
    code.time = 830 + 23 / 30.0; // CLV 0:13:50, picture 24
    code.chap = 3;
    code.clv = true;
    code.resolution = 1.0 / 30;
    frame.setDiscInfo(&code, 30000, 1001);
    CHECK(frame.disc_frame == 830 * 30 + 23);
    CHECK(frame.chapter == 3);
    CHECK(frame.flags == (c_flag_clv_known | c_flag_clv | c_flag_frame_resolution));

    code.time = 12 * 60; // a time code of minutes only
    code.resolution = 60;
    code.lead_in = true;
    frame.setDiscInfo(&code, 25, 1);
    CHECK(frame.disc_frame == 12 * 60 * 25);
    CHECK(frame.flags == (c_flag_clv_known | c_flag_clv | c_flag_lead_in));

    frame.setDiscInfo(nullptr, 25, 1);
    CHECK(frame.disc_frame == -1);
    CHECK(frame.chapter == -1);
    CHECK(frame.flags == 0);
}

TEST_CASE("signature: a file reads back as written", "[signature]") {
    TempFile file;
    const Header header = Header::forStandard(Standard::ePal, 25, 1);
    std::vector<Frame> frames(3);
    for (int i = 0; i < 3; i++) {
        frames[i].disc_frame = i == 1 ? -1 : 5000 + i;
        frames[i].chapter = (int16_t)(i - 1);
        frames[i].flags = (uint8_t)(c_flag_clv_known | (i ? c_flag_frame_resolution : 0));
        for (int b = 0; b < c_block_count; b++)
            frames[i].blocks[b] = -0.2f + 0.004f * b + 0.1f * i; // -0.2 to 1.5: sync-ward of blanking, above white
    }
    {
        Writer writer(file.path, header);
        for (const auto &frame : frames)
            writer.append(frame);
        CHECK(writer.good());
        CHECK(writer.frameCount() == 3);
    }
    CHECK(std::filesystem::file_size(file.path) == c_header_size + 3 * c_record_size);
    const Recording recording = read(file.path);
    CHECK(recording.header == header);
    REQUIRE(recording.frames.size() == 3);
    for (int i = 0; i < 3; i++) {
        CHECK(recording.frames[i].disc_frame == frames[i].disc_frame);
        CHECK(recording.frames[i].chapter == frames[i].chapter);
        CHECK(recording.frames[i].flags == frames[i].flags);
        for (int b = 0; b < c_block_count; b++)
            CHECK(std::abs(recording.frames[i].blocks[b] - frames[i].blocks[b]) < 1.0f / 32768);
    }
}

TEST_CASE("signature: a truncated record is dropped and a foreign file refused", "[signature]") {
    TempFile file;
    {
        Writer writer(file.path, Header::forStandard(Standard::eMuse, 30, 1));
        writer.append(Frame{});
        writer.append(Frame{});
    }
    std::filesystem::resize_file(file.path, c_header_size + c_record_size + 100);
    CHECK(read(file.path).frames.size() == 1);
    { std::ofstream(file.path, std::ios::binary | std::ios::trunc) << std::string(200, 'x'); }
    CHECK_THROWS_AS(read(file.path), std::runtime_error);
}
