// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)
//
// A benchmark behind a planned libFLAC patch: the order-12 LPC restore (the
// decoder's prediction step, 44% of the decode time of a 10-bit capture on
// ARM64, where libFLAC 1.5.0 has no NEON decoder code) as libFLAC's unrolled C
// runs it, against two NEON versions.  Sample by sample, NEON gains almost
// nothing (5%): each output depends on the previous twelve, so the loop is a
// serial recurrence and the horizontal add sits on its critical path.  Four
// outputs per step gain 2.85x on an Apple M2 (7.7 to 2.7 ns per sample): the
// part of each of the four sums that uses samples before the group is a dot
// product over known values, built in parallel with one lane per output, and
// a short serial tail adds the six terms that use the group's own outputs.
// Bit-identical to the C loop.  Orders below 12 fit the same code with the
// coefficients zero-padded; the 64-bit "wide" path (full 16-bit captures at
// high coefficient precision) is a separate case.
//
//   clang++ -O3 -std=c++20 -o lpc-neon-bench tools/lpc-neon-bench.cpp && ./lpc-neon-bench
//
#include <arm_neon.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

__attribute__((noinline)) void restore_c(const int32_t *residual, uint32_t data_len, const int32_t *qlp_coeff, int lp_quantization, int32_t *data) {
    for (int i = 0; i < (int)data_len; i++) {
        int32_t sum = 0;
        sum += qlp_coeff[11] * data[i-12];
        sum += qlp_coeff[10] * data[i-11];
        sum += qlp_coeff[9] * data[i-10];
        sum += qlp_coeff[8] * data[i-9];
        sum += qlp_coeff[7] * data[i-8];
        sum += qlp_coeff[6] * data[i-7];
        sum += qlp_coeff[5] * data[i-6];
        sum += qlp_coeff[4] * data[i-5];
        sum += qlp_coeff[3] * data[i-4];
        sum += qlp_coeff[2] * data[i-3];
        sum += qlp_coeff[1] * data[i-2];
        sum += qlp_coeff[0] * data[i-1];
        data[i] = residual[i] + (sum >> lp_quantization);
    }
}

__attribute__((noinline)) void restore_neon12(const int32_t *residual, uint32_t data_len, const int32_t *qlp_coeff, int lp_quantization, int32_t *data) {
    // coefficients in history order: lane k of c0 multiplies data[i-12+k], etc.
    const int32x4_t c0 = {qlp_coeff[11], qlp_coeff[10], qlp_coeff[9], qlp_coeff[8]};
    const int32x4_t c1 = {qlp_coeff[7], qlp_coeff[6], qlp_coeff[5], qlp_coeff[4]};
    const int32x4_t c2 = {qlp_coeff[3], qlp_coeff[2], qlp_coeff[1], qlp_coeff[0]};
    int32x4_t h0 = vld1q_s32(data - 12), h1 = vld1q_s32(data - 8), h2 = vld1q_s32(data - 4);
    for (int i = 0; i < (int)data_len; i++) {
        int32x4_t p = vmulq_s32(c0, h0);
        p = vmlaq_s32(p, c1, h1);
        p = vmlaq_s32(p, c2, h2);
        const int32_t sum = vaddvq_s32(p);
        const int32_t out = residual[i] + (sum >> lp_quantization);
        data[i] = out;
        h0 = vextq_s32(h0, h1, 1);
        h1 = vextq_s32(h1, h2, 1);
        h2 = vextq_s32(h2, vdupq_n_s32(out), 1);
    }
}


// Four outputs per step: the part of each sum that uses samples before the
// group is a dot product over known values, so the four sums are built in
// parallel, one lane per output, with twelve multiply-adds of a history value
// against the four coefficients it meets; then a short serial tail adds the
// terms that use the group's own outputs (six multiply-adds).
__attribute__((noinline)) void restore_neon12x4(const int32_t *residual, uint32_t data_len, const int32_t *qlp_coeff, int lp_quantization, int32_t *data) {
    // cv[k]: the coefficients history value data[i-12+k] meets in outputs i..i+3
    int32x4_t cv[12];
    for (int k = 0; k < 12; k++) {
        int32_t lanes[4];
        for (int m = 0; m < 4; m++) {
            // output i+m meets data[i-12+k] as data[(i+m)-1-j] with j = 11-k+m; the
            // terms with j past the order come from the group's own outputs, in the tail
            const int idx = 11 - k + m;
            lanes[m] = idx <= 11 ? qlp_coeff[idx] : 0;
        }
        cv[k] = vld1q_s32(lanes);
    }
    const int32_t c0 = qlp_coeff[0], c1 = qlp_coeff[1], c2 = qlp_coeff[2];
    int i = 0;
    for (; i + 4 <= (int)data_len; i += 4) {
        const int32_t *h = data + i - 12;
        int32x4_t s = vmulq_s32(cv[0], vdupq_n_s32(h[0]));
        s = vmlaq_s32(s, cv[1], vdupq_n_s32(h[1]));
        s = vmlaq_s32(s, cv[2], vdupq_n_s32(h[2]));
        s = vmlaq_s32(s, cv[3], vdupq_n_s32(h[3]));
        s = vmlaq_s32(s, cv[4], vdupq_n_s32(h[4]));
        s = vmlaq_s32(s, cv[5], vdupq_n_s32(h[5]));
        s = vmlaq_s32(s, cv[6], vdupq_n_s32(h[6]));
        s = vmlaq_s32(s, cv[7], vdupq_n_s32(h[7]));
        s = vmlaq_s32(s, cv[8], vdupq_n_s32(h[8]));
        s = vmlaq_s32(s, cv[9], vdupq_n_s32(h[9]));
        s = vmlaq_s32(s, cv[10], vdupq_n_s32(h[10]));
        s = vmlaq_s32(s, cv[11], vdupq_n_s32(h[11]));
        const int32_t s0 = vgetq_lane_s32(s, 0), s1 = vgetq_lane_s32(s, 1), s2 = vgetq_lane_s32(s, 2), s3 = vgetq_lane_s32(s, 3);
        const int32_t o0 = residual[i] + (s0 >> lp_quantization);
        const int32_t o1 = residual[i + 1] + ((s1 + c0 * o0) >> lp_quantization);
        const int32_t o2 = residual[i + 2] + ((s2 + c1 * o0 + c0 * o1) >> lp_quantization);
        const int32_t o3 = residual[i + 3] + ((s3 + c2 * o0 + c1 * o1 + c0 * o2) >> lp_quantization);
        data[i] = o0; data[i + 1] = o1; data[i + 2] = o2; data[i + 3] = o3;
    }
    for (; i < (int)data_len; i++) {
        int32_t sum = 0;
        for (int j = 0; j < 12; j++)
            sum += qlp_coeff[j] * data[i - j - 1];
        data[i] = residual[i] + (sum >> lp_quantization);
    }
}

int main() {
    constexpr uint32_t n = 4096, blocks = 20000;
    std::mt19937 rng(1);
    std::vector<int32_t> coeff(12), residual(n), a(n + 12), b(n + 12);
    for (auto &c : coeff) c = (int32_t)(rng() % 32768) - 16384;
    for (auto &r : residual) r = (int32_t)(rng() % 64) - 32;
    for (int k = 0; k < 12; k++) a[k] = b[k] = (int32_t)(rng() % 1024) - 512;
    const int q = 14;
    auto bench = [&](auto fn, std::vector<int32_t> &buf, const char *name) {
        const auto t0 = std::chrono::steady_clock::now();
        for (uint32_t k = 0; k < blocks; k++)
            fn(residual.data(), n, coeff.data(), q, buf.data() + 12);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        printf("%-8s %.1f ns/sample, %.0f Msamples/s\n", name, s / blocks / n * 1e9, blocks * (double)n / s / 1e6);
    };
    bench(restore_c, a, "C");
    bench(restore_neon12, b, "NEON");
    bench(restore_c, a, "C");
    bench(restore_neon12, b, "NEON");
    std::vector<int32_t> c(n + 12); for (int k = 0; k < 12; k++) c[k] = a[k];
    bench(restore_neon12x4, c, "NEONx4");
    bench(restore_c, a, "C");
    bench(restore_neon12x4, c, "NEONx4");
    printf("x4 identical: %s\n", memcmp(a.data(), c.data(), (n + 12) * sizeof(int32_t)) == 0 ? "yes" : "NO");
    printf("identical: %s\n", memcmp(a.data(), b.data(), (n + 12) * sizeof(int32_t)) == 0 ? "yes" : "NO");
}
