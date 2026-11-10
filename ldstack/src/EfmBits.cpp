// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)
//
// ldstack efmbits: align the predicted EFM channel bits of a disc (from its
// known audio) with the transition stream a capture's EFM decoder produced
// (ac3rf-efm-decode --t-values-output-filename), following bit clock slips
// window by window, and tabulate how the transitions were deformed.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "CircEncoder.h"
#include "EfmTable.h"

using namespace std;

namespace {
    constexpr int c_frame_bits = 588;

    struct Predicted {
        vector<uint8_t> toggle; // 1 where a transition is predicted
        vector<uint8_t> known;  // 0 for merging bits and the subcode symbol
        int64_t g0;             // frame index of bit 0
    };

    Predicted predictBits(const EncodedFrames &enc, int64_t g0, int64_t g1) {
        Predicted p;
        p.g0 = g0;
        p.toggle.assign((size_t)(g1 - g0) * c_frame_bits, 0);
        p.known.assign(p.toggle.size(), 1);
        for (int64_t g = g0; g < g1; g++) {
            uint8_t *t = p.toggle.data() + (g - g0) * c_frame_bits;
            uint8_t *k = p.known.data() + (g - g0) * c_frame_bits;
            // Sync 100000000001000000000010 (transitions at bits 0, 11, 22) + 3 merging
            t[0] = t[11] = t[22] = 1;
            k[24] = k[25] = k[26] = 0;
            int pos = 27;
            // Subcode symbol: unknown
            for (int i = 0; i < 17; i++) k[pos + i] = 0;
            pos += 17;
            const bool valid = g >= enc.first_valid && g < (int64_t)enc.frames.size();
            for (int s = 0; s < 32; s++) {
                if (valid) {
                    const uint16_t code = c_byte_to_efm_toggles[enc.frames[g][s]];
                    for (int b = 0; b < 14; b++) t[pos + b] = (code >> (13 - b)) & 1;
                } else
                    for (int b = 0; b < 14; b++) k[pos + b] = 0;
                k[pos + 14] = k[pos + 15] = k[pos + 16] = 0;
                pos += 17;
            }
        }
        return p;
    }

    // Received transitions: bit positions, from the run lengths
    vector<int64_t> readTransitions(const string &path) {
        FILE *f = fopen(path.c_str(), "rb");
        if (f == nullptr) throw runtime_error("cannot open " + path);
        vector<int64_t> pos;
        int64_t bit = 0;
        uint8_t buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0)
            for (size_t i = 0; i < n; i++) { bit += buf[i]; pos.push_back(bit); }
        fclose(f);
        return pos;
    }

    // Score of predicted window [w0, w1) placed at received bit offset `off`
    // (received bit = predicted bit + off): known predicted transitions that
    // have a received transition at exactly that bit
    int scoreAt(const Predicted &p, const vector<uint8_t> &rbits, int64_t w0, int64_t w1, int64_t off) {
        int s = 0;
        for (int64_t i = w0; i < w1; i++) {
            if (!p.known[i] || !p.toggle[i]) continue;
            const int64_t r = i + off;
            if (r >= 0 && r < (int64_t)rbits.size() && rbits[r]) s++;
        }
        return s;
    }
}

int cmdEfmBits(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: ldstack efmbits <audio.pcm> <tvalues.u8> <phase> <received_frame:predicted_frame> "
                        "[--window BITS] [--search BITS] [--min-score F] [--trace FILE]\n");
        return 1;
    }
    const int phase = atoi(argv[4]);
    long long hint_m, hint_g;
    if (sscanf(argv[5], "%lld:%lld", &hint_m, &hint_g) != 2) throw runtime_error("bad hint");
    int window = 1176, search = 48;
    int64_t start_search = 6000;
    double min_score = 0.3;
    string trace;
    for (int i = 6; i < argc; i++) {
        const string a = argv[i];
        if (a == "--start-search" && i + 1 < argc) start_search = atoll(argv[++i]);
        else if (a == "--window" && i + 1 < argc) window = atoi(argv[++i]);
        else if (a == "--search" && i + 1 < argc) search = atoi(argv[++i]);
        else if (a == "--min-score" && i + 1 < argc) min_score = atof(argv[++i]);
        else if (a == "--trace" && i + 1 < argc) trace = argv[++i];
    }
    // Audio
    vector<int16_t> pcm;
    {
        FILE *f = fopen(argv[2], "rb");
        if (f == nullptr) throw runtime_error("cannot open audio");
        int16_t buf[4096]; size_t n;
        while ((n = fread(buf, sizeof(int16_t), 4096, f)) > 0) pcm.insert(pcm.end(), buf, buf + n);
        fclose(f);
    }
    const EncodedFrames enc = circEncode(pcm.data(), (int64_t)pcm.size() / 2, phase);
    const Predicted pred = predictBits(enc, 0, (int64_t)enc.frames.size());
    const vector<int64_t> rtrans = readTransitions(argv[3]);
    const int64_t rbits_n = rtrans.empty() ? 0 : rtrans.back() + 1;
    vector<uint8_t> rbits((size_t)rbits_n, 0);
    for (int64_t t : rtrans) rbits[(size_t)t] = 1;
    printf("%zu predicted frames (%lld valid), %lld received bits, %zu received transitions\n", enc.frames.size(),
           (long long)(enc.frames.size() - enc.first_valid), (long long)rbits_n, rtrans.size());

    // Initial offset from the hint: received frame m ~ received bit 588 m
    // (the t-values start at the decoder's first bit), predicted frame g
    int64_t off = hint_m * c_frame_bits - hint_g * c_frame_bits;
    {
        const int64_t w0 = hint_g * c_frame_bits, w1 = w0 + 4 * c_frame_bits;
        int best = -1; int64_t boff = off;
        for (int64_t d = -start_search; d <= start_search; d++) {
            const int s = scoreAt(pred, rbits, w0, w1, off + d);
            if (s > best) { best = s; boff = off + d; }
        }
        int known = 0;
        for (int64_t i = w0; i < w1; i++) known += pred.known[i] && pred.toggle[i];
        printf("start: offset %lld, %d of %d predicted transitions found\n", (long long)boff, best, known);
        off = boff;
    }

    // Track forward from the hint window (and backward), tabulating
    FILE *tf = trace.empty() ? nullptr : fopen(trace.c_str(), "w");
    if (tf) fprintf(tf, "# predicted_bit\treceived_offset\tscore\tknown\tstate\n");
    // Deformation tables
    int64_t n_hit = 0, n_shift[2] = {0, 0}, n_miss = 0, n_spurious = 0, n_known_toggles = 0, n_received_in_windows = 0;
    // by predicted run length (min of the two adjacent runs) 3..11: found / total
    int64_t by_t_found[12] = {}, by_t_total[12] = {};
    // predicted run length T vs received run length (both ends found): [T][Trec]
    int64_t run_matrix[12][16] = {};
    // by polarity class (parity of the predicted transition count) and T: sum of (received - predicted) length
    double pol_sum[2][12] = {}; int64_t pol_n[2][12] = {};
    int64_t windows = 0, windows_lost = 0;
    int64_t last_off = off;
    int lost_run = 0;
    for (int64_t w0 = hint_g * c_frame_bits; w0 + window <= (int64_t)pred.toggle.size(); w0 += window) {
        const int64_t w1 = w0 + window;
        int known = 0;
        for (int64_t i = w0; i < w1; i++) known += pred.known[i] && pred.toggle[i];
        if (known == 0) break;
        const int64_t s_range = lost_run > 0 ? min<int64_t>(4000, search * (1 + 4 * lost_run)) : search;
        int best = -1; int64_t boff = last_off;
        for (int64_t d = -s_range; d <= s_range; d++) {
            const int s = scoreAt(pred, rbits, w0, w1, last_off + d);
            if (s > best || (s == best && llabs(d) < llabs(boff - last_off))) { best = s; boff = last_off + d; }
        }
        if (boff + w1 >= rbits_n) break;
        const bool ok = (double)best / known >= min_score;
        windows++;
        if (tf) fprintf(tf, "%lld\t%lld\t%.3f\t%d\t%s\n", (long long)w0, (long long)boff, (double)best / known, known, ok ? "ok" : "lost");
        if (!ok) { windows_lost++; lost_run++; continue; }
        lost_run = 0;
        last_off = boff;
        // Tabulate this window.  Transition polarity class: parity of the
        // number of predicted transitions before it (all bits, unknown ones
        // count as predicted; the class is only meaningful as a consistent
        // label within a stretch, which is all the asymmetry test needs)
        // Precompute matched received position for every predicted known toggle
        vector<int64_t> pt;   // predicted toggle bits in window (known)
        vector<int64_t> rm;   // matched received bit or -1
        for (int64_t i = w0; i < w1; i++) {
            if (!pred.known[i] || !pred.toggle[i]) continue;
            const int64_t r = i + boff;
            int64_t m = -1;
            if (rbits[r]) m = r;
            else if (rbits[r - 1] && !rbits[r + 1]) m = r - 1;
            else if (rbits[r + 1] && !rbits[r - 1]) m = r + 1;
            pt.push_back(i); rm.push_back(m);
        }
        n_known_toggles += pt.size();
        for (size_t j = 0; j < pt.size(); j++) {
            if (rm[j] < 0) { n_miss++; }
            else if (rm[j] == pt[j] + boff) n_hit++;
            else n_shift[rm[j] < pt[j] + boff ? 0 : 1]++;
        }
        // Spurious: received transitions in the window not within +-1 of any predicted known toggle,
        // counted only over stretches where the prediction is known on both sides
        for (int64_t r = w0 + boff; r < w1 + boff; r++) {
            if (!rbits[r]) continue;
            n_received_in_windows++;
            const int64_t i = r - boff;
            bool near = false;
            for (int64_t d = -1; d <= 1; d++) {
                const int64_t q = i + d;
                if (q >= w0 && q < w1 && pred.known[q] && pred.toggle[q]) near = true;
                if (q >= w0 && q < w1 && !pred.known[q]) near = true; // unknown bits: cannot judge
            }
            if (!near) n_spurious++;
        }
        // Runs between consecutive predicted known toggles with only known bits between
        for (size_t j = 0; j + 1 < pt.size(); j++) {
            bool all_known = true;
            for (int64_t i = pt[j] + 1; i < pt[j + 1]; i++) if (!pred.known[i]) { all_known = false; break; }
            if (!all_known) continue;
            const int T = (int)(pt[j + 1] - pt[j]);
            if (T < 3 || T > 11) continue;
            // detection by run length: the transition at pt[j+1] and pt[j]
            by_t_total[T] += 2;
            by_t_found[T] += (rm[j] >= 0) + (rm[j + 1] >= 0);
            if (rm[j] >= 0 && rm[j + 1] >= 0) {
                const int Tr = (int)(rm[j + 1] - rm[j]);
                if (Tr >= 0 && Tr < 16) run_matrix[T][Tr]++;
                // polarity: count predicted toggles (known or not) from the start of the frame
                int64_t cnt = 0;
                const int64_t f0 = (pt[j] / c_frame_bits) * c_frame_bits;
                for (int64_t i = f0; i < pt[j]; i++) cnt += pred.toggle[i] | (pred.known[i] ? 0 : 0);
                const int cls = (int)(cnt & 1);
                pol_sum[cls][T] += Tr - T; pol_n[cls][T]++;
            }
        }
    }
    if (tf) fclose(tf);
    printf("windows: %lld, lost: %lld (%.1f%%)\n", (long long)windows, (long long)windows_lost, 100.0 * windows_lost / max<int64_t>(1, windows));
    const double tot = (double)max<int64_t>(1, n_known_toggles);
    printf("predicted transitions in aligned windows: %lld; found in place %.2f%%, shifted early %.2f%%, shifted late %.2f%%, missing %.2f%%\n",
           (long long)n_known_toggles, 100 * n_hit / tot, 100 * n_shift[0] / tot, 100 * n_shift[1] / tot, 100 * n_miss / tot);
    printf("received transitions in aligned windows: %lld, spurious (no prediction within +-1 bit): %lld (%.2f%%)\n",
           (long long)n_received_in_windows, (long long)n_spurious, 100.0 * n_spurious / max<int64_t>(1, n_received_in_windows));
    printf("transition detection by adjacent run length T:");
    for (int T = 3; T <= 11; T++) if (by_t_total[T]) printf(" T%d:%.1f%%", T, 100.0 * by_t_found[T] / by_t_total[T]);
    printf("\nreceived run length given predicted T (rows T3..T11, columns received 2..13, %% of row):\n");
    for (int T = 3; T <= 11; T++) {
        int64_t rs = 0; for (int c = 0; c < 16; c++) rs += run_matrix[T][c];
        if (!rs) continue;
        printf("  T%-2d n=%-8lld", T, (long long)rs);
        for (int c = 2; c <= 13; c++) printf(" %5.1f", 100.0 * run_matrix[T][c] / rs);
        printf("\n");
    }
    printf("mean received-minus-predicted run length by polarity class and T (bits):\n");
    for (int cls = 0; cls < 2; cls++) {
        printf("  class %d:", cls);
        for (int T = 3; T <= 11; T++) if (pol_n[cls][T]) printf(" T%d:%+.3f", T, pol_sum[cls][T] / pol_n[cls][T]);
        printf("\n");
    }
    return 0;
}

// ldstack efmexport <audio.pcm> <phase> <g0> <g1> <out>: the predicted channel
// bits of frames g0..g1 as two byte arrays, <out>.toggle.u8 and <out>.known.u8
int cmdEfmExport(int argc, char **argv) {
    if (argc < 7) { fprintf(stderr, "usage: ldstack efmexport <audio.pcm> <phase> <g0> <g1> <out>\n"); return 1; }
    vector<int16_t> pcm;
    FILE *f = fopen(argv[2], "rb");
    if (f == nullptr) throw runtime_error("cannot open audio");
    int16_t buf[4096]; size_t n;
    while ((n = fread(buf, sizeof(int16_t), 4096, f)) > 0) pcm.insert(pcm.end(), buf, buf + n);
    fclose(f);
    const EncodedFrames enc = circEncode(pcm.data(), (int64_t)pcm.size() / 2, atoi(argv[3]));
    const int64_t g0 = atoll(argv[4]), g1 = min<int64_t>(atoll(argv[5]), (int64_t)enc.frames.size());
    const Predicted p = predictBits(enc, g0, g1);
    const string out = argv[6];
    FILE *t = fopen((out + ".toggle.u8").c_str(), "wb"), *k = fopen((out + ".known.u8").c_str(), "wb");
    if (!t || !k) throw runtime_error("cannot write " + out);
    fwrite(p.toggle.data(), 1, p.toggle.size(), t); fwrite(p.known.data(), 1, p.known.size(), k);
    fclose(t); fclose(k);
    printf("%lld frames, %zu bits written\n", (long long)(g1 - g0), p.toggle.size());
    return 0;
}
