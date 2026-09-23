// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "CircEncoder.h"

#include <stdexcept>
#include "rs/GFValue.h"

namespace {
    using GF = GfValueHelper::GFValue<8, 0x11d, 2>;

    // Fill the four symbols at positions `p` so that the n-symbol word has
    // zero syndromes for roots alpha^0 .. alpha^3 (fcr 0), with the
    // decoder's convention that the first symbol is the highest-order
    // coefficient: syndrome r = sum_j w[j] * alpha^(r * (n - 1 - j))
    template<int n>
    void fillParity(std::array<uint8_t, n> &word, const std::array<int, 4> &p) {
        GF a[4][4], b[4];
        for (int r = 0; r < 4; r++) {
            GF s(0);
            for (int col = 0; col < n; col++) {
                bool unknown = false;
                for (int u = 0; u < 4; u++) if (p[u] == col) unknown = true;
                if (!unknown) s = s + GF::alpha_pow(r * (n - 1 - col)) * GF(word[col]);
            }
            b[r] = s; // sum over unknowns must equal s (characteristic 2: minus is plus)
            for (int u = 0; u < 4; u++) a[r][u] = GF::alpha_pow(r * (n - 1 - p[u]));
        }
        // Gaussian elimination
        for (int c = 0; c < 4; c++) {
            int piv = -1;
            for (int r = c; r < 4; r++) if (a[r][c].getInt() != 0) { piv = r; break; }
            if (piv < 0) throw std::runtime_error("singular parity system");
            if (piv != c) { for (int k = 0; k < 4; k++) std::swap(a[piv][k], a[c][k]); std::swap(b[piv], b[c]); }
            const GF inv = a[c][c].inverse();
            for (int k = 0; k < 4; k++) a[c][k] = a[c][k] * inv;
            b[c] = b[c] * inv;
            for (int r = 0; r < 4; r++) {
                if (r == c || a[r][c].getInt() == 0) continue;
                const GF f = a[r][c];
                for (int k = 0; k < 4; k++) a[r][k] = a[r][k] + f * a[c][k];
                b[r] = b[r] + f * b[c];
            }
        }
        for (int u = 0; u < 4; u++) word[p[u]] = (uint8_t)b[u].getInt();
    }
}

EncodedFrames circEncode(const int16_t *samples, int64_t count, int phase) {
    EncodedFrames out;
    const int64_t nf = (count - phase) / 6;
    if (nf < 120) throw std::runtime_error("too few samples to encode");

    // Stage A: 24 bytes per frame.  The decoder maps output bytes to samples
    // as L0 = 0,1  L1 = 12,13  L2 = 2,3  L3 = 14,15  L4 = 4,5  L5 = 16,17 and
    // R0 = 6,7  R1 = 18,19  R2 = 8,9  R3 = 20,21  R4 = 10,11  R5 = 22,23
    static const int l_hi[6] = {0, 12, 2, 14, 4, 16}, r_hi[6] = {6, 18, 8, 20, 10, 22};
    std::vector<std::array<uint8_t, 24>> a(nf);
    for (int64_t f = 0; f < nf; f++)
        for (int i = 0; i < 6; i++) {
            const int16_t l = samples[2 * (6 * f + phase + i)], r = samples[2 * (6 * f + phase + i) + 1];
            a[f][l_hi[i]] = (uint8_t)((uint16_t)l >> 8); a[f][l_hi[i] + 1] = (uint8_t)(l & 0xff);
            a[f][r_hi[i]] = (uint8_t)((uint16_t)r >> 8); a[f][r_hi[i] + 1] = (uint8_t)(r & 0xff);
        }
    // Stage B: the decoder delays output bytes 12..23 by two frames, so the
    // encoder delays 0..11 by two.  Stage C: C2 parity at 12..15.
    std::vector<std::array<uint8_t, 28>> c2(nf);
    for (int64_t f = 2; f < nf; f++) {
        for (int i = 0; i < 12; i++) c2[f][i] = a[f - 2][i];
        for (int i = 12; i < 24; i++) c2[f][i + 4] = a[f][i];
        fillParity<28>(c2[f], {12, 13, 14, 15});
    }
    // Stage D: the decoder delays c1 symbol i by (27 - i) * 4 frames before
    // C2, so the encoder delays c2 symbol i by 4 i.  Stage E: C1 parity at 28..31.
    std::vector<std::array<uint8_t, 32>> c1(nf);
    for (int64_t f = 2 + 108; f < nf; f++) {
        for (int i = 0; i < 28; i++) c1[f][i] = c2[f - 4 * i][i];
        fillParity<32>(c1[f], {28, 29, 30, 31});
    }
    // Stage F: the decoder delays odd symbols by one frame, so the encoder
    // delays the even ones; parity symbols 12..15 and 28..31 are inverted
    out.first_valid = 2 + 108 + 1;
    out.frames.resize(nf);
    for (int64_t f = out.first_valid; f < nf; f++)
        for (int i = 0; i < 32; i++) {
            uint8_t v = c1[f - (1 - i % 2)][i];
            if ((i >= 12 && i < 16) || i >= 28) v = (uint8_t)~v;
            out.frames[f][i] = v;
        }
    return out;
}
