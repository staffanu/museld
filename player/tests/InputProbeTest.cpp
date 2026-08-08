// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>
#include "InputProbe.h"
#include "logging/StreamLogger.h"

using namespace std;

namespace {

// An FM video carrier at base_hz with deviation dev_hz, driven by a synthetic
// line-periodic pattern (sync tip plus non-harmonic in-line content, so the
// only strong periodicity is the line itself), with an optional unmodulated
// audio carrier and a little noise.
vector<float> synthesizeRf(size_t n, double fs, double line_hz,
                           double base_hz, double dev_hz, double audio_hz) {
    vector<float> out(n);
    double phase = 0, audio_phase = 0;
    mt19937 rng(1234);
    uniform_real_distribution<float> noise(-0.05f, 0.05f);
    const double line_period = fs / line_hz;
    for (size_t i = 0; i < n; i++) {
        double p = fmod((double)i, line_period) / line_period;
        double video = p < 0.08 ? -1.0 : 0.4 * sin(2 * M_PI * 1.3 * p);
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

constexpr size_t c_samples = 6 << 20;

} // namespace

TEST_CASE("Probe detects synthetic NTSC RF in an extension-less s16 file", "[InputProbe]") {
    auto path = tempFile("museld-probe-ntsc.bin");
    writeS16(path, synthesizeRf(c_samples, 40e6, 15734.2657, 8.2e6, 0.9e6, 2.301e6));
    auto r = probe(path);
    filesystem::remove(path);

    REQUIRE(r.format.has_value());
    CHECK(*r.format == eSint16);
    CHECK(r.type == InputProbeResult::Type::eNtscRf);
    CHECK_THAT(r.sample_frequency, Catch::Matchers::WithinRel(40e6, 0.001));
    CHECK(r.audio_carrier_ratio > 50);
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
    auto samples = synthesizeRf(c_samples, 40e6, 15734.2657, 8.2e6, 0.9e6, 2.301e6);

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
