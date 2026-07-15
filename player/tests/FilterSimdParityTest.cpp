// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

// SIMD/scalar parity tests for the FIR filter stages.
//
// Notes on FirFilterStage padding: when use_simd is true, FirFilterStage pads
// the reversed filter with trailing zeros so its length becomes a multiple of
// the SIMD chunk width.  Those zero taps live at the *low-index* end of the
// stored (reversed) buffer, which corresponds to extra delay at the *front*
// of the original impulse response — i.e. the SIMD path computes a
// fractionally-shifted convolution compared to the scalar path when the
// original filter length is not already a multiple of the chunk width.
// Tests that compare FirFilterStage SIMD against scalar therefore use filter
// lengths that are already a multiple of 8, so padding is a no-op.

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <random>
#include <vector>

#include "filter/ComplexFirFilterStage.h"
#include "filter/FirFilterStage.h"
#include "filter/HalfBand.h"

namespace {

constexpr float kTolerance = 1e-4f;
// Generous upper bound on any history we need to prefill.
constexpr int kPrefillHistory = 64;

std::vector<float> randomVector(int n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> v(n);
    for (auto &x : v) x = dist(rng);
    return v;
}

// Fill a stage's input buffer so that its `history` leading slots hold
// stream[fresh_offset - history .. fresh_offset - 1] and the remaining
// `n_input` slots hold stream[fresh_offset .. fresh_offset + n_input - 1].
// This makes outputs from stages with different filter sizes (e.g. due to
// SIMD padding) directly comparable.
void fillBuffer(std::vector<float> *buf, int history, int n_input,
                const std::vector<float> &stream, int fresh_offset) {
    REQUIRE(static_cast<int>(buf->size()) == history + n_input);
    REQUIRE(fresh_offset >= history);
    REQUIRE(static_cast<int>(stream.size()) >= fresh_offset + n_input);
    std::copy(stream.begin() + fresh_offset - history,
              stream.begin() + fresh_offset + n_input,
              buf->begin());
}

std::vector<float> runFirStage(const std::vector<float> &filter, int decimation,
                               int n_input, const std::vector<float> &stream,
                               int fresh_offset, bool use_simd) {
    std::vector<float> output(n_input / decimation, 0.0f);
    FirFilterStage stage("test", "test", filter, decimation, n_input,
                         /*output_offset=*/0, &output, use_simd);
    fillBuffer(stage.inputBuffer(), stage.filterSize() - 1, n_input,
               stream, fresh_offset);
    stage.applyFilter(n_input);
    return output;
}

void requireClose(const std::vector<float> &a, const std::vector<float> &b,
                  float tol = kTolerance) {
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        INFO("index " << i << " a=" << a[i] << " b=" << b[i]);
        REQUIRE(std::abs(a[i] - b[i]) <= tol);
    }
}

// Symmetric filter of a chosen length, hand-crafted (not a real DSP design)
// just to exercise the filter pipeline.  Returned filter length = ntaps.
std::vector<float> makeArbitraryFilter(int ntaps, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    std::vector<float> f(ntaps);
    for (auto &x : f) x = dist(rng);
    return f;
}

} // namespace

// ---------- FirFilterStage SIMD vs scalar ----------

TEST_CASE("FirFilterStage SIMD matches scalar - decim=1, length multiple of 8") {
    if (!FirFilterStage::simdSupported()) {
        SUCCEED();
        return;
    }
    const auto filter = makeArbitraryFilter(24, 42);
    const int n_input = 128;
    const auto stream = randomVector(kPrefillHistory + n_input, 0xBEEF);
    auto out_simd = runFirStage(filter, 1, n_input, stream, kPrefillHistory, true);
    auto out_ref  = runFirStage(filter, 1, n_input, stream, kPrefillHistory, false);
    requireClose(out_simd, out_ref);
}

TEST_CASE("FirFilterStage SIMD matches scalar - decim=2, length multiple of 8") {
    if (!FirFilterStage::simdSupported()) {
        SUCCEED();
        return;
    }
    for (int ntaps : {16, 24, 32}) {
        const auto filter = makeArbitraryFilter(ntaps, 0x123 + ntaps);
        const int n_input = 64;
        const auto stream = randomVector(kPrefillHistory + n_input, 0xC0FFEEu + ntaps);
        auto out_simd = runFirStage(filter, 2, n_input, stream, kPrefillHistory, true);
        auto out_ref  = runFirStage(filter, 2, n_input, stream, kPrefillHistory, false);
        requireClose(out_simd, out_ref);
    }
}

// ---------- ComplexFirFilterStage SIMD vs scalar ----------

TEST_CASE("ComplexFirFilterStage SIMD matches scalar") {
    if (!FirFilterStage::simdSupported()) {
        SUCCEED();
        return;
    }
    const auto filter = makeArbitraryFilter(24, 7);
    const int n_input = 64;
    const auto re_stream = randomVector(kPrefillHistory + n_input, 1);
    const auto im_stream = randomVector(kPrefillHistory + n_input, 2);

    auto run = [&](bool use_simd) {
        std::vector<float> re_out(n_input / 2, 0.0f);
        std::vector<float> im_out(n_input / 2, 0.0f);
        ComplexFirFilterStage stage("c", "c", filter, 2, n_input, 0,
                                    &re_out, &im_out, use_simd);
        const int history = stage.filterSize() - 1;
        fillBuffer(stage.inputReBuffer(), history, n_input, re_stream, kPrefillHistory);
        fillBuffer(stage.inputImBuffer(), history, n_input, im_stream, kPrefillHistory);
        stage.applyFilter(n_input);
        return std::pair{re_out, im_out};
    };

    auto [re_simd, im_simd] = run(true);
    auto [re_ref,  im_ref]  = run(false);
    requireClose(re_simd, re_ref);
    requireClose(im_simd, im_ref);
}
