// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include "input/PrefetchingInputReader.h"

namespace {

// Emits f[i] = stream position + i, so every delivered block states where it came from and
// continuity across reads and seeks can be checked exactly.  Positions stay well below 2^24
// in the tests, where float holds integers exactly.
class CountingReader : public InputReader {
public:
    CountingReader(uint32_t block_size, off_t total_samples,
                   std::chrono::microseconds read_delay = std::chrono::microseconds(0),
                   off_t throw_at = -1)
        : InputReader(-1, block_size, false),
          m_total(total_samples),
          m_delay(read_delay),
          m_throw_at(throw_at) {}

    void initialize() override { initialized = true; }

    void seek(off_t no_samples) override {
        std::scoped_lock<std::mutex> lock(m_fd_mutex);
        REQUIRE(m_position + no_samples >= 0); // the adapter clamps before forwarding
        m_position += no_samples;
    }

    int readFloats(float *f) override {
        std::scoped_lock<std::mutex> lock(m_fd_mutex);
        if (m_delay.count() > 0)
            std::this_thread::sleep_for(m_delay);
        if (m_throw_at >= 0 && m_position >= m_throw_at)
            throw std::runtime_error("injected read failure");
        if (m_position + (off_t)m_block_size > m_total)
            return 0;
        for (uint32_t i = 0; i < m_block_size; i++)
            f[i] = (float)(m_position + (off_t)i);
        m_position += m_block_size;
        return (int)m_block_size;
    }

    int bitsPerSample() const override { return 12; }

    void setDcBlocking(bool enabled) override { dc_blocking = enabled; }

    bool initialized = false;
    bool dc_blocking = false;

private:
    const off_t m_total;
    const std::chrono::microseconds m_delay;
    const off_t m_throw_at;
    off_t m_position = 0;
};

// Signed: seek arguments like -2 * c_block must not wrap in unsigned arithmetic
constexpr off_t c_block = 256;

// Read one block and require it to start at `expected_start` and count up from there.
void requireBlockAt(PrefetchingInputReader &reader, off_t expected_start) {
    std::vector<float> f(c_block);
    REQUIRE(reader.readFloats(f.data()) == (int)c_block);
    for (off_t i = 0; i < c_block; i++) {
        if (f[i] != (float)(expected_start + i)) {
            CAPTURE(expected_start, i);
            REQUIRE(f[i] == (float)(expected_start + i));
        }
    }
}

} // namespace

TEST_CASE("Blocks arrive in order and end with EOF", "[PrefetchingInputReader]") {
    constexpr off_t c_blocks = 20;
    PrefetchingInputReader reader(std::make_unique<CountingReader>(c_block, c_blocks * c_block));
    reader.initialize();
    for (off_t b = 0; b < c_blocks; b++)
        requireBlockAt(reader, b * c_block);
    std::vector<float> f(c_block);
    REQUIRE(reader.readFloats(f.data()) == 0);
    REQUIRE(reader.readFloats(f.data()) == 0); // EOF persists
}

TEST_CASE("initialize and setDcBlocking reach the inner reader", "[PrefetchingInputReader]") {
    auto inner = std::make_unique<CountingReader>(c_block, 4 * c_block);
    CountingReader *inner_raw = inner.get();
    PrefetchingInputReader reader(std::move(inner));
    reader.setDcBlocking(true);
    REQUIRE(reader.bitsPerSample() == 12);
    REQUIRE(reader.block_size() == c_block);
    reader.initialize();
    REQUIRE(inner_raw->initialized);
    REQUIRE(inner_raw->dc_blocking);
}

TEST_CASE("Seeks compensate for the read-ahead", "[PrefetchingInputReader]") {
    constexpr off_t c_blocks = 40;
    PrefetchingInputReader reader(std::make_unique<CountingReader>(c_block, c_blocks * c_block));
    reader.initialize();

    off_t position = 0;
    for (int b = 0; b < 3; b++, position += c_block)
        requireBlockAt(reader, position);

    // Let the producer fill its queue so there is read-ahead to compensate for
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    SECTION("forward, not a multiple of the block size") {
        reader.seek(3 * c_block + 17);
        position += 3 * c_block + 17;
    }
    SECTION("backward") {
        reader.seek(-2 * c_block);
        position -= 2 * c_block;
    }
    SECTION("backward past the start clamps to it") {
        reader.seek(-1000 * c_block);
        position = 0;
    }
    for (int b = 0; b < 3; b++, position += c_block)
        requireBlockAt(reader, position);
}

TEST_CASE("A backward seek after EOF resumes reading", "[PrefetchingInputReader]") {
    constexpr off_t c_blocks = 5;
    PrefetchingInputReader reader(std::make_unique<CountingReader>(c_block, c_blocks * c_block));
    reader.initialize();
    for (off_t b = 0; b < c_blocks; b++)
        requireBlockAt(reader, b * c_block);
    std::vector<float> f(c_block);
    REQUIRE(reader.readFloats(f.data()) == 0);

    reader.seek(-2 * c_block);
    requireBlockAt(reader, (c_blocks - 2) * c_block);
    requireBlockAt(reader, (c_blocks - 1) * c_block);
    REQUIRE(reader.readFloats(f.data()) == 0);
}

TEST_CASE("A seek before initialize passes through unadjusted", "[PrefetchingInputReader]") {
    PrefetchingInputReader reader(std::make_unique<CountingReader>(c_block, 10 * c_block));
    reader.seek(c_block + 3);
    reader.initialize();
    requireBlockAt(reader, c_block + 3);
}

TEST_CASE("A producer exception surfaces after the good blocks", "[PrefetchingInputReader]") {
    constexpr off_t c_good_blocks = 5;
    PrefetchingInputReader reader(std::make_unique<CountingReader>(
        c_block, 100 * c_block, std::chrono::microseconds(0), c_good_blocks * c_block));
    reader.initialize();
    for (off_t b = 0; b < c_good_blocks; b++)
        requireBlockAt(reader, b * c_block);
    std::vector<float> f(c_block);
    REQUIRE_THROWS_AS(reader.readFloats(f.data()), std::runtime_error);
}

TEST_CASE("Destruction is safe at any point", "[PrefetchingInputReader]") {
    SECTION("before initialize") {
        PrefetchingInputReader reader(std::make_unique<CountingReader>(c_block, 10 * c_block));
    }
    SECTION("right after initialize") {
        PrefetchingInputReader reader(std::make_unique<CountingReader>(
            c_block, 1000 * c_block, std::chrono::microseconds(200)));
        reader.initialize();
    }
    SECTION("mid-stream") {
        PrefetchingInputReader reader(std::make_unique<CountingReader>(
            c_block, 1000 * c_block, std::chrono::microseconds(200)));
        reader.initialize();
        requireBlockAt(reader, 0);
    }
}

TEST_CASE("Reads interleaved with seeks under a slow producer", "[PrefetchingInputReader]") {
    constexpr off_t c_blocks = 400; // the net forward drift of the seek pattern stays well inside this
    PrefetchingInputReader reader(std::make_unique<CountingReader>(
        c_block, c_blocks * c_block, std::chrono::microseconds(100)));
    reader.initialize();

    off_t position = 0;
    for (int round = 0; round < 50; round++) {
        for (int b = 0; b < 3; b++, position += c_block)
            requireBlockAt(reader, position);
        off_t delta = (round % 2 == 0) ? 5 * c_block + round : -(2 * c_block + round);
        if (position + delta < 0)
            delta = -position;
        reader.seek(delta);
        position += delta;
    }
}
