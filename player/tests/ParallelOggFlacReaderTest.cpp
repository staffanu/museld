// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

// ParallelOggFlacReader against libFLAC's own encoder: the samples come out in
// order and identical to the serial reader's, on any number of threads, across
// seeks and up to the end.

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <unistd.h>
#include <FLAC++/encoder.h>
#include "input/ByteSource.h"
#include "input/LdfInputReader.h"
#include "input/ParallelOggFlacReader.h"

namespace {

constexpr int c_block = 4096;

// Deterministic, index-addressable noise with some structure, so FLAC's
// predictors have something to do and the pages are not all alike
int16_t sampleAt(int64_t i) {
    const int64_t noise = (int64_t)(((uint64_t)i * 2654435761ULL) >> 20) & 0x3FF;
    return (int16_t)(noise + (i % 777) * 8 - 3000);
}

std::string encodeOgg(int64_t n, int blocksize = c_block) {
    static int counter = 0;
    const auto path = (std::filesystem::temp_directory_path()
                       / std::format("museld-pogg-test-{}-{}.ldf", ::getpid(), counter++)).string();
    FLAC::Encoder::File enc;
    enc.set_channels(1);
    enc.set_bits_per_sample(16);
    enc.set_sample_rate(40000);
    enc.set_blocksize((unsigned)blocksize);
    enc.set_streamable_subset(false); // the subset caps the block size at 4608 for 40 kHz
    enc.set_ogg_serial_number(4321);
    REQUIRE(enc.init_ogg(path.c_str()) == FLAC__STREAM_ENCODER_INIT_STATUS_OK);
    std::vector<FLAC__int32> buf((size_t)blocksize);
    for (int64_t i = 0; i < n;) {
        const int count = (int)std::min<int64_t>(blocksize, n - i);
        for (int k = 0; k < count; k++)
            buf[k] = sampleAt(i + k);
        REQUIRE(enc.process_interleaved(buf.data(), count));
        i += count;
    }
    REQUIRE(enc.finish());
    return path;
}

struct TempFile {
    std::string path;
    explicit TempFile(std::string p) : path(std::move(p)) {}
    ~TempFile() { std::filesystem::remove(path); }
};

// Reads one block and checks it against the expected sample index
void expectBlock(InputReader &reader, int64_t at, uint32_t block) {
    std::vector<float> f(block);
    REQUIRE(reader.readFloats(f.data()) == (int)block);
    for (uint32_t i = 0; i < block; i++) {
        if (f[i] != (float)sampleAt(at + (int64_t)i)) {
            FAIL(std::format("sample {} is {} and should be {}", at + i, f[i], sampleAt(at + i)));
        }
    }
}

} // namespace

TEST_CASE("The parallel reader decodes the whole stream in order, on 1, 2 and 3 threads", "[ParallelOggFlacReader]") {
    constexpr int64_t n = 3'000'000; // several 2 MiB runs
    constexpr uint32_t block = 100'000;
    TempFile file(encodeOgg(n));
    for (int threads : {1, 2, 3}) {
        ParallelOggFlacReader reader(openByteSource(file.path), block, threads);
        reader.initialize();
        REQUIRE(reader.sampleCount() == n);
        REQUIRE(reader.bitsPerSample() == 16);
        int64_t at = 0;
        while (at + block <= n) {
            expectBlock(reader, at, block);
            at += block;
        }
        std::vector<float> f(block);
        REQUIRE(reader.readFloats(f.data()) == 0); // a partial block is the end
        REQUIRE(reader.readFloats(f.data()) == 0);
    }
}

TEST_CASE("The parallel reader's output equals the serial reader's", "[ParallelOggFlacReader]") {
    constexpr int64_t n = 2'500'000;
    constexpr uint32_t block = 65536;
    TempFile file(encodeOgg(n));
    LdfInputReader serial(openByteSource(file.path), block, eFlacOgg);
    ParallelOggFlacReader parallel(openByteSource(file.path), block, 2);
    serial.initialize();
    parallel.initialize();
    serial.setDcBlocking(true);
    parallel.setDcBlocking(true);
    std::vector<float> a(block), b(block);
    int blocks = 0;
    for (;;) {
        const int ra = serial.readFloats(a.data()), rb = parallel.readFloats(b.data());
        REQUIRE(ra == rb);
        if (ra == 0) break;
        REQUIRE(a == b);
        blocks++;
    }
    REQUIRE(blocks == (int)(n / block));
}

TEST_CASE("Seeks land on the sample they name", "[ParallelOggFlacReader]") {
    constexpr int64_t n = 3'000'000;
    constexpr uint32_t block = 50'000;
    TempFile file(encodeOgg(n));
    ParallelOggFlacReader reader(openByteSource(file.path), block, 2);
    reader.initialize();
    int64_t at = 0;
    expectBlock(reader, at, block); at += block;
    REQUIRE(reader.seek(2'000'000)); at += 2'000'000;     // far forward, past what is queued
    expectBlock(reader, at, block); at += block;
    REQUIRE(reader.seek(-1'500'000)); at -= 1'500'000;    // backwards
    expectBlock(reader, at, block); at += block;
    REQUIRE(reader.seek(12'345)); at += 12'345;           // a short hop, mid-frame
    expectBlock(reader, at, block); at += block;
    REQUIRE(reader.seek(-at)); at = 0;                    // to the start
    expectBlock(reader, at, block); at += block;
    REQUIRE(reader.seek(-10'000'000)); at = 0;            // clamped at the start
    expectBlock(reader, at, block); at += block;
    REQUIRE(reader.seek(n - block - at)); at = n - block; // the last whole block
    expectBlock(reader, at, block); at += block;
    std::vector<float> f(block);
    REQUIRE(reader.readFloats(f.data()) == 0);
    REQUIRE(!reader.seek(n));                             // beyond the end: refused, position kept
    REQUIRE(reader.seek(-(int64_t)block * 2)); at -= block * 2;
    expectBlock(reader, at, block);
}

TEST_CASE("A seek while runs are in flight does not deliver stale samples", "[ParallelOggFlacReader]") {
    constexpr int64_t n = 4'000'000;
    constexpr uint32_t block = 4096;
    TempFile file(encodeOgg(n));
    ParallelOggFlacReader reader(openByteSource(file.path), block, 3);
    reader.initialize();
    int64_t at = 0;
    for (int round = 0; round < 40; round++) {
        expectBlock(reader, at, block); at += block;
        const int64_t hop = (round * 977'131) % 1'900'000 - 900'000;
        const int64_t target = std::clamp<int64_t>(at + hop, 0, n - 2 * block);
        REQUIRE(reader.seek(target - at));
        at = target;
    }
}

TEST_CASE("Seeks into frames that span pages land on the sample they name", "[ParallelOggFlacReader]") {
    // Frames of 32768 samples are 60 KB packets, longer than an Ogg page can
    // hold, so most pages continue a packet from the page before: the run of
    // a seek has to start on the page where the target's frame begins, not on
    // the continuation page that ends it (libogg drops a partial first packet,
    // and playback would begin a frame late)
    constexpr int64_t n = 2'000'000;
    constexpr uint32_t block = 10'000;
    TempFile file(encodeOgg(n, 32768));
    ParallelOggFlacReader reader(openByteSource(file.path), block, 2);
    reader.initialize();
    int64_t at = 0;
    for (int round = 0; round < 60; round++) {
        const int64_t target = ((int64_t)round * 1'234'567 + 5'000) % (n - 2 * block);
        REQUIRE(reader.seek(target - at));
        at = target;
        expectBlock(reader, at, block); at += block;
    }
}
