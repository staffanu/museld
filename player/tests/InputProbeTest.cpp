// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>
#include "InputProbe.h"
#include "input/PreparedInput.h"
#include "logging/StreamLogger.h"

using namespace std;

namespace {

// An FM video carrier at base_hz with deviation dev_hz, driven by a synthetic
// line-periodic pattern (sync tip plus non-harmonic in-line content, so the
// only strong periodicity is the line itself), with an optional unmodulated
// audio carrier and a little noise.  With lines_per_field set (262.5 NTSC,
// 312.5 PAL), every field starts with three lines of sync tip, standing in
// for the vertical sync's broad pulses.
vector<float> synthesizeRf(size_t n, double fs, double line_hz,
                           double base_hz, double dev_hz, double audio_hz,
                           double lines_per_field = 0) {
    vector<float> out(n);
    double phase = 0, audio_phase = 0;
    mt19937 rng(1234);
    uniform_real_distribution<float> noise(-0.05f, 0.05f);
    const double line_period = fs / line_hz;
    const double field_period = lines_per_field * line_period;
    for (size_t i = 0; i < n; i++) {
        double p = fmod((double)i, line_period) / line_period;
        double video = p < 0.08 ? -1.0 : 0.4 * sin(2 * M_PI * 1.3 * p);
        if (field_period > 0 && fmod((double)i, field_period) < 3 * line_period)
            video = -1.0;
        phase += 2 * M_PI * (base_hz + dev_hz * video) / fs;
        float s = (float)(0.8 * cos(phase));
        if (audio_hz > 0) {
            audio_phase += 2 * M_PI * audio_hz / fs;
            s += (float)(0.25 * cos(audio_phase));
        }
        out[i] = s + noise(rng);
    }
    return out;
}

filesystem::path tempFile(const string &name) {
    return filesystem::temp_directory_path() / name;
}

void writeS16(const filesystem::path &path, const vector<float> &samples) {
    ofstream f(path, ios::binary);
    for (float s : samples) {
        auto v = (int16_t)lround(s * 20000.0f);
        f.write((const char *)&v, sizeof(v));
    }
}

void writeU8(const filesystem::path &path, const vector<float> &samples) {
    ofstream f(path, ios::binary);
    for (float s : samples) {
        auto v = (uint8_t)clamp((int)lround(s * 100.0f) + 128, 0, 255);
        f.write((const char *)&v, sizeof(v));
    }
}

// The DomesDay Duplicator packing: four 10-bit samples in five bytes
void writeLds(const filesystem::path &path, const vector<float> &samples) {
    ofstream f(path, ios::binary);
    for (size_t i = 0; i + 4 <= samples.size(); i += 4) {
        uint32_t v[4];
        for (int k = 0; k < 4; k++)
            v[k] = (uint32_t)clamp((int)lround(samples[i + k] * 400.0f) + 512, 0, 1023);
        uint8_t packed[5] = {
            (uint8_t)(v[0] >> 2),
            (uint8_t)((v[0] << 6) | (v[1] >> 4)),
            (uint8_t)((v[1] << 4) | (v[2] >> 6)),
            (uint8_t)((v[2] << 2) | (v[3] >> 8)),
            (uint8_t)v[3],
        };
        f.write((const char *)packed, sizeof(packed));
    }
}

InputProbeResult probe(const filesystem::path &path) {
    StreamLogger log(StreamLogger::c_log_warn, cerr, false);
    return probeInputFile(log, path.string(), nullopt);
}

// A live input, as a pipe is to the probe: bytes arrive in pieces, cannot be
// seeked, have no size, and once they are gone they are gone.
class LiveBytes : public ByteSource {
public:
    explicit LiveBytes(vector<uint8_t> bytes) : m_bytes(std::move(bytes)) {}

    ssize_t read(uint8_t *buf, size_t len) override {
        const size_t n = min({len, m_bytes.size() - m_pos, (size_t)65521}); // odd pieces, like a pipe
        memcpy(buf, m_bytes.data() + m_pos, n);
        m_pos += n;
        return (ssize_t)n;
    }
    int64_t seek(int64_t, int) override {
        errno = ESPIPE;
        return -1;
    }
    int64_t size() override { return -1; }
    bool isLive() const override { return true; }

private:
    vector<uint8_t> m_bytes;
    size_t m_pos = 0;
};

vector<uint8_t> s16Bytes(const vector<float> &samples) {
    vector<uint8_t> bytes;
    bytes.reserve(samples.size() * 2);
    for (float s : samples) {
        auto v = (int16_t)lround(s * 20000.0f);
        bytes.push_back((uint8_t)(v & 0xFF));
        bytes.push_back((uint8_t)((uint16_t)v >> 8));
    }
    return bytes;
}

constexpr size_t c_samples = 6 << 20;

} // namespace

TEST_CASE("Probe detects synthetic NTSC RF in an extension-less s16 file", "[InputProbe]") {
    auto path = tempFile("museld-probe-ntsc.bin");
    writeS16(path, synthesizeRf(c_samples, 40e6, 15734.2657, 8.2e6, 0.9e6, 2.301e6, 262.5));
    auto r = probe(path);
    filesystem::remove(path);

    REQUIRE(r.format.has_value());
    CHECK(*r.format == eSint16);
    CHECK(r.type == InputProbeResult::Type::eNtscRf);
    CHECK_THAT(r.sample_frequency, Catch::Matchers::WithinRel(40e6, 0.001));
    CHECK(r.audio_carrier_ratio > 50);
    CHECK_THAT(r.field_lines, Catch::Matchers::WithinAbs(262.5, 1.0));
}

TEST_CASE("Probe tells PAL RF from NTSC by the lines between vertical syncs", "[InputProbe]") {
    // PAL's carrier band overlaps NTSC's and its line rate is 0.7 % off, so
    // only the field length can tell them apart; no audio carrier, as on a
    // digital-sound PAL disc
    auto path = tempFile("museld-probe-pal.bin");
    writeS16(path, synthesizeRf(c_samples, 40e6, 15625.0, 7.3e6, 0.57e6, 0, 312.5));
    auto r = probe(path);
    filesystem::remove(path);

    REQUIRE(r.format.has_value());
    CHECK(r.type == InputProbeResult::Type::ePalRf);
    CHECK_THAT(r.sample_frequency, Catch::Matchers::WithinRel(40e6, 0.001));
    CHECK_THAT(r.field_lines, Catch::Matchers::WithinAbs(312.5, 1.0));
}

TEST_CASE("Probe falls back on the audio carrier when no vertical sync is seen", "[InputProbe]") {
    // A purely line-periodic signal has no field structure; the NTSC analog
    // audio carrier still says NTSC
    auto path = tempFile("museld-probe-ntsc-nofield.bin");
    writeS16(path, synthesizeRf(c_samples, 40e6, 15734.2657, 8.2e6, 0.9e6, 2.301e6));
    auto r = probe(path);
    filesystem::remove(path);

    CHECK(r.type == InputProbeResult::Type::eNtscRf);
    CHECK(r.field_lines == 0);
}

TEST_CASE("Probe detects synthetic MUSE RF and its sample rate", "[InputProbe]") {
    auto path = tempFile("museld-probe-muse.bin");
    writeS16(path, synthesizeRf(c_samples, 62.5e6, 33750.0, 11.5e6, 2e6, 0));
    auto r = probe(path);
    filesystem::remove(path);

    REQUIRE(r.format.has_value());
    CHECK(*r.format == eSint16);
    CHECK(r.type == InputProbeResult::Type::eMuseRf);
    CHECK_THAT(r.sample_frequency, Catch::Matchers::WithinRel(62.5e6, 0.001));
}

TEST_CASE("Probe detects u8 and lds sample formats by content", "[InputProbe]") {
    auto samples = synthesizeRf(c_samples, 40e6, 15734.2657, 8.2e6, 0.9e6, 2.301e6, 262.5);

    auto u8_path = tempFile("museld-probe-u8.bin");
    writeU8(u8_path, samples);
    auto u8_result = probe(u8_path);
    filesystem::remove(u8_path);
    REQUIRE(u8_result.format.has_value());
    CHECK(*u8_result.format == eUint8);
    CHECK(u8_result.type == InputProbeResult::Type::eNtscRf);

    auto lds_path = tempFile("museld-probe-lds.bin");
    writeLds(lds_path, samples);
    auto lds_result = probe(lds_path);
    filesystem::remove(lds_path);
    REQUIRE(lds_result.format.has_value());
    CHECK(*lds_result.format == eLds);
    CHECK(lds_result.type == InputProbeResult::Type::eNtscRf);
}

TEST_CASE("Probe detects muse-16 baseband by its folded line profile", "[InputProbe]") {
    // MUSE baseband is hostile to autocorrelation: the dot-interleaved video
    // decorrelates adjacent lines, leaving only a small per-line sync.  Model
    // that as content smooth within each 480-sample line but independent from
    // line to line, and expect the fold-480 test to find the sync anyway.
    vector<float> samples(c_samples);
    mt19937 rng(3);
    uniform_real_distribution<float> cycles(3.0f, 40.0f);
    uniform_real_distribution<float> phase01(0.0f, 1.0f);
    for (size_t line = 0; line * 480 < samples.size(); line++) {
        float f = cycles(rng), ph = phase01(rng);
        for (int i = 0; i < 480 && line * 480 + i < samples.size(); i++)
            samples[line * 480 + i] = i < 12
                    ? (i < 6 ? -0.5f : 0.5f)
                    : 0.3f * sinf(2 * (float)M_PI * (f * i / 480.0f + ph));
    }

    auto path = tempFile("museld-probe-m16.bin");
    writeS16(path, samples);
    auto r = probe(path);
    filesystem::remove(path);

    CHECK(r.type == InputProbeResult::Type::eMuse16Baseband);
    CHECK(r.sample_frequency == 16.2e6);
}

TEST_CASE("Probe reports white noise as unclassifiable", "[InputProbe]") {
    vector<float> noise(c_samples);
    mt19937 rng(7);
    uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (auto &s : noise) s = dist(rng);

    auto path = tempFile("museld-probe-noise.bin");
    writeS16(path, noise);
    auto r = probe(path);
    filesystem::remove(path);

    CHECK(r.type == InputProbeResult::Type::eUnknown);
}

TEST_CASE("Probe reads a live input window after window until the signal starts", "[InputProbe]") {
    // A capture device started before PLAY: 12M samples of noise, then NTSC
    // RF.  The probe has to wait through the noise without committing to a
    // format or a type, and the player then has to continue from the reader
    // the probe decoded the stream with -- there is no reopening a pipe.
    mt19937 rng(11);
    uniform_real_distribution<float> dist(-0.1f, 0.1f);
    vector<float> samples(12 << 20);
    for (auto &s : samples) s = dist(rng);
    const auto rf = synthesizeRf(c_samples, 40e6, 15734.2657, 8.2e6, 0.9e6, 2.301e6, 262.5);
    samples.insert(samples.end(), rf.begin(), rf.end());
    // and a marker the player can recognize after the handover, starting on a
    // block boundary of the player's reader so a lost sample would show as a
    // misaligned marker
    REQUIRE(samples.size() % 4096 == 0);
    for (int i = 0; i < 1 << 20; i++)
        samples.push_back(i % 2 ? 1.0f : -1.0f);

    StreamLogger log(StreamLogger::c_log_warn, cerr, false);
    PreparedInput input("live", make_unique<LiveBytes>(s16Bytes(samples)));
    REQUIRE(input.isLive());
    auto r = probeInput(log, input, nullopt);

    REQUIRE(r.format.has_value());
    CHECK(*r.format == eSint16);
    CHECK(r.type == InputProbeResult::Type::eNtscRf);
    CHECK_THAT(r.sample_frequency, Catch::Matchers::WithinRel(40e6, 0.001));
    CHECK(r.line_strength > 0.6); // measured on a clean window, not the one the signal starts in

    // The player opens the input in its own block size and reads on to the marker
    auto reader = input.open(4096, nullptr);
    reader->initialize();
    CHECK(reader->isLive());
    // The reader starts somewhere inside the RF (the probe consumed the rest),
    // and the marker has to turn up whole, at the start of a block: RF never
    // alternates between exactly the two extremes
    vector<float> block(4096);
    auto markerAt = [&](size_t i) {
        for (size_t k = 0; k < 8; k++)
            if (block[i + k] != ((i + k) % 2 ? 20000.0f : -20000.0f))
                return false;
        return true;
    };
    bool marker_seen = false, marker_aligned = false;
    while (!marker_seen && reader->readFloats(block.data()) == 4096) {
        for (size_t i = 0; i + 8 <= block.size(); i++) {
            if (markerAt(i)) {
                marker_seen = true;
                marker_aligned = i == 0;
                break;
            }
        }
    }
    CHECK(marker_seen);
    CHECK(marker_aligned);
}

TEST_CASE("Probe takes a live input of known format from its first window", "[InputProbe]") {
    const auto rf = synthesizeRf(c_samples, 62.5e6, 33750.0, 11.5e6, 2e6, 0);
    StreamLogger log(StreamLogger::c_log_warn, cerr, false);
    PreparedInput input("live", make_unique<LiveBytes>(s16Bytes(rf)), eSint16);
    auto r = probeInput(log, input, eSint16);
    CHECK(r.type == InputProbeResult::Type::eMuseRf);
    CHECK_THAT(r.sample_frequency, Catch::Matchers::WithinRel(62.5e6, 0.001));
}
