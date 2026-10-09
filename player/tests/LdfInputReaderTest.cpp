// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <catch2/catch_all.hpp>
#include <FLAC++/encoder.h>
#include <format>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "input/ByteSource.h"
#include "input/LdfInputReader.h"

namespace {
    constexpr uint32_t c_block = 4096;

    // Deterministic, index-addressable noise
    int16_t sampleAt(int64_t i) {
        return (int16_t)(((uint64_t)i * 2654435761ULL) >> 13);
    }

    std::string tempName(const char *tag) {
        static int counter = 0;
        return (std::filesystem::temp_directory_path()
                / std::format("museld-ldf-test-{}-{}-{}.bin", ::getpid(), tag, counter++)).string();
    }

    // Encodes n samples with libFLAC, plain or in Ogg.  The file encoder
    // writes the true count into STREAMINFO when it finishes.
    std::string encode(int64_t n, bool ogg) {
        FLAC::Encoder::File enc;
        enc.set_channels(1);
        enc.set_bits_per_sample(16);
        enc.set_sample_rate(40000);
        enc.set_blocksize(c_block);
        if (ogg)
            enc.set_ogg_serial_number(1234);
        const auto path = tempName(ogg ? "ogg" : "flac");
        const auto status = ogg ? enc.init_ogg(path.c_str()) : enc.init(path.c_str());
        REQUIRE(status == FLAC__STREAM_ENCODER_INIT_STATUS_OK);
        std::vector<FLAC__int32> buf(c_block);
        for (int64_t i = 0; i < n;) {
            const int count = (int)std::min<int64_t>(c_block, n - i);
            for (int k = 0; k < count; k++)
                buf[k] = sampleAt(i + k);
            REQUIRE(enc.process_interleaved(buf.data(), count));
            i += count;
        }
        REQUIRE(enc.finish());
        return path;
    }

    std::vector<uint8_t> readAll(const std::string &path) {
        std::ifstream f(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(f), {}};
    }

    void writeAll(const std::string &path, const std::vector<uint8_t> &bytes) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write((const char *)bytes.data(), (std::streamsize)bytes.size());
    }

    uint32_t oggCrc(const uint8_t *p, size_t n) {
        uint32_t crc = 0;
        for (size_t i = 0; i < n; i++) {
            crc ^= (uint32_t)p[i] << 24;
            for (int b = 0; b < 8; b++)
                crc = crc & 0x80000000u ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
        }
        return crc;
    }

    // Rewrites the STREAMINFO sample count (36 bits) in a plain or Ogg file,
    // with a valid page checksum in the Ogg case, as an encoder with a
    // wrapped or unknown count would have written it
    void setStreamInfoTotal(const std::string &path, bool ogg, uint64_t total) {
        auto bytes = readAll(path);
        const size_t streaminfo = ogg ? 28 + 17 : 8;
        uint8_t *field = bytes.data() + streaminfo + 13;
        field[0] = (uint8_t)((field[0] & 0xF0) | ((total >> 32) & 0x0F));
        for (int k = 1; k <= 4; k++)
            field[k] = (uint8_t)(total >> (8 * (4 - k)));
        if (ogg) {
            const size_t page = 28 + 51;
            for (int k = 0; k < 4; k++) bytes[22 + k] = 0;
            const uint32_t crc = oggCrc(bytes.data(), page);
            for (int k = 0; k < 4; k++) bytes[22 + k] = (uint8_t)(crc >> (8 * k));
        }
        writeAll(path, bytes);
    }

    std::unique_ptr<LdfInputReader> open(const std::string &path, bool ogg) {
        auto reader = std::make_unique<LdfInputReader>(openByteSource(path), c_block, ogg ? eFlacOgg : eFlac);
        reader->initialize();
        return reader;
    }

    // Reads one block at the current position and checks it against the generator
    void expectBlockAt(LdfInputReader &reader, int64_t position) {
        std::vector<float> f(c_block);
        REQUIRE(reader.readFloats(f.data()) == (int)c_block);
        for (uint32_t k = 0; k < c_block; k += 97)
            REQUIRE(f[k] == (float)sampleAt(position + k));
    }

    struct Cleanup {
        std::string path;
        ~Cleanup() { std::remove(path.c_str()); }
    };
}

TEST_CASE("FLAC sample count and seeking", "[ldf]") {
    const int64_t n = 50 * c_block + 1234;
    const bool ogg = GENERATE(false, true);
    const auto path = encode(n, ogg);
    Cleanup cleanup{path};

    SECTION("as encoded") {
        auto reader = open(path, ogg);
        CHECK(reader->sampleCount() == n);
        REQUIRE(reader->seek(n - 3 * c_block));
        expectBlockAt(*reader, n - 3 * c_block);
    }

    SECTION("count unknown (zero), as a pipe-written file has it") {
        setStreamInfoTotal(path, ogg, 0);
        auto reader = open(path, ogg);
        // From the file's tail: the last Ogg page's granule position, or the
        // last frame header of a plain stream
        CHECK(reader->sampleCount() == n);
        REQUIRE(reader->seek(n - 3 * c_block));
        expectBlockAt(*reader, n - 3 * c_block);
    }

    SECTION("count wrapped to a small value, as a long capture has it") {
        setStreamInfoTotal(path, ogg, 1000);
        auto reader = open(path, ogg);
        // libFLAC would refuse every seek past sample 1000 if it saw the field
        REQUIRE(reader->seek(n - 3 * c_block));
        expectBlockAt(*reader, n - 3 * c_block);
        CHECK(reader->sampleCount() == n);
    }

    SECTION("a refused seek leaves the reader where it was") {
        auto reader = open(path, ogg);
        REQUIRE(reader->seek(10 * c_block));
        expectBlockAt(*reader, 10 * c_block);
        CHECK_FALSE(reader->seek(n + 100 * c_block)); // past the end
        expectBlockAt(*reader, 11 * c_block);
    }
}
