// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)
//
// ldstack efmscore: score the symbol decisions in an ac3rf-efm-decode
// --symbol-debug-filename dump against the stream predicted from the known
// audio: how many table hits are wrong, how often the ML estimate is right
// on a miss, and how well its score margin separates right from wrong.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "CircEncoder.h"
#include "EfmTable.h"

using namespace std;

namespace {
    struct Slot { bool hit; uint8_t table, best, second; double p_best, p_second; uint16_t efm; };
    // Predicted channel bits of one frame: toggle and known flags (merging
    // bits and the subcode symbol unknown), same layout as the decoder
    void frameBits(const std::array<uint8_t, 32> &syms, uint8_t *tog, uint8_t *known) {
        for (int i = 0; i < 588; i++) { tog[i] = 0; known[i] = 1; }
        tog[0] = tog[11] = tog[22] = 1; known[24] = known[25] = known[26] = 0;
        for (int i = 27; i < 44; i++) known[i] = 0;
        for (int s = 0; s < 32; s++) {
            const int pos = 27 + 17 * (s + 1); const uint16_t code = c_byte_to_efm_toggles[syms[s]];
            for (int b = 0; b < 14; b++) tog[pos + b] = (code >> (13 - b)) & 1;
            known[pos + 14] = known[pos + 15] = known[pos + 16] = 0;
        }
    }
    // Best shift k in -3..3 such that the 14 bits read equal the predicted bits at pos+k on the known bits; 99 = none
    int bestShift(uint16_t efm, const uint8_t *tog, const uint8_t *known, int pos) {
        for (int mag = 0; mag <= 3; mag++)
            for (int k : {mag, -mag}) {
                bool ok = true;
                for (int b = 0; b < 14 && ok; b++) {
                    const int q = pos + b + k;
                    if (q < 0 || q >= 588 || !known[q]) continue;
                    if (((efm >> (13 - b)) & 1) != tog[q]) ok = false;
                }
                if (ok) return k;
                if (mag == 0) break;
            }
        return 99;
    }
    struct Frame { Slot s[33]; bool seen[33]; };

    vector<int16_t> readPcm(const string &path) {
        FILE *f = fopen(path.c_str(), "rb");
        if (f == nullptr) throw runtime_error("cannot open " + path);
        vector<int16_t> v; int16_t buf[4096]; size_t n;
        while ((n = fread(buf, sizeof(int16_t), 4096, f)) > 0) v.insert(v.end(), buf, buf + n);
        fclose(f);
        if (v.size() % 2) v.pop_back();
        return v;
    }
    void percentiles(vector<double> v, const char *label) {
        if (v.empty()) { printf("  %s: none\n", label); return; }
        sort(v.begin(), v.end());
        auto p = [&](double q) { return v[min(v.size() - 1, (size_t)(q * v.size()))]; };
        printf("  %-34s n=%-8zu p10 %.3f  p50 %.3f  p90 %.3f\n", label, v.size(), p(0.1), p(0.5), p(0.9));
    }
}

int cmdEfmScore(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: ldstack efmscore <audio.pcm> <symbols.f32> [--phase P] [--range MIN-MAX]\n"); return 1; }
    int only_phase = -1, local = 0, blocks = 0; int64_t dmin = -3000, dmax = 3000, force_offset = 1 << 30;
    for (int i = 4; i < argc; i++) {
        const string a = argv[i];
        if (a == "--phase" && i + 1 < argc) only_phase = atoi(argv[++i]);
        else if (a == "--local" && i + 1 < argc) local = atoi(argv[++i]);
        else if (a == "--offset" && i + 1 < argc) force_offset = atoll(argv[++i]);
        else if (a == "--blocks" && i + 1 < argc) blocks = atoi(argv[++i]);
        else if (a == "--range" && i + 1 < argc) { long long x, y; if (sscanf(argv[++i], "%lld-%lld", &x, &y) == 2) { dmin = x; dmax = y; } }
    }
    // Records
    FILE *f = fopen(argv[3], "rb");
    if (f == nullptr) throw runtime_error("cannot open dump");
    map<int64_t, Frame> frames;
    float rec[24];
    while (fread(rec, sizeof rec, 1, f) == 1) {
        const int64_t fr = (int64_t)rec[0]; const int slot = (int)rec[1];
        if (slot < 0 || slot > 32) continue;
        Frame &F = frames[fr];
        F.s[slot] = {rec[2] > 0.5f, (uint8_t)rec[3], (uint8_t)rec[4], (uint8_t)rec[5], rec[7], rec[8], (uint16_t)rec[6]};
        F.seen[slot] = true;
    }
    fclose(f);
    if (frames.empty()) throw runtime_error("no records");
    vector<int64_t> keys; for (auto &[k, v] : frames) keys.push_back(k);
    printf("%zu frames of symbol records\n", frames.size());

    const auto pcm = readPcm(argv[2]);
    vector<EncodedFrames> preds;
    for (int phase = 0; phase < 6; phase++) preds.push_back(circEncode(pcm.data(), (int64_t)pcm.size() / 2, phase));
    // Alignment on the table hits: received frame m (dump frame count) <-> predicted frame m + delta
    auto score = [&](const EncodedFrames &p, int64_t delta, size_t k0, size_t k1) {
        int64_t match = 0, total = 0;
        for (size_t k = k0; k < k1; k++) {
            const int64_t g = keys[k] + delta;
            if (g < p.first_valid || g >= (int64_t)p.frames.size()) continue;
            const Frame &F = frames[keys[k]];
            for (int s = 1; s <= 32; s++) {
                if (!F.seen[s] || !F.s[s].hit) continue;
                total++; if (F.s[s].table == p.frames[g][s - 1]) match++;
            }
        }
        return make_pair(match, total);
    };
    if (local > 0) {
        // Block-wise alignment: where the decoder's stream follows the prediction and where it slips
        printf("block_start_frame\tphase\toffset\tmatch\thits\n");
        for (size_t b = 0; b + local <= keys.size(); b += local) {
            double bb = -1; int bp = 0; int64_t bd = 0, bt = 0;
            for (int phase = 0; phase < 6; phase++) {
                if (only_phase >= 0 && phase != only_phase) continue;
                for (int64_t d = dmin; d <= dmax; d++) {
                    auto [m, t] = score(preds[phase], d, b, b + local);
                    if (t > 50 && (double)m / t > bb) { bb = (double)m / t; bp = phase; bd = d; bt = t; }
                }
            }
            printf("%lld\t%d\t%+lld\t%.3f\t%lld\n", (long long)keys[b], bp, (long long)bd, bb, (long long)bt);
        }
        return 0;
    }
    const size_t k0 = keys.size() / 2, k1 = min(keys.size(), k0 + 300);
    double best = -1; int best_phase = 0; int64_t best_delta = 0;
    if (force_offset != (1 << 30)) { best_phase = max(0, only_phase); best_delta = force_offset; best = 1; }
    for (int phase = 0; phase < 6 && force_offset == (1 << 30); phase++) {
        if (only_phase >= 0 && phase != only_phase) continue;
        for (int64_t d = dmin; d <= dmax; d++) {
            auto [m, t] = score(preds[phase], d, k0, k1);
            if (t > 500 && (double)m / t > best) { best = (double)m / t; best_phase = phase; best_delta = d; }
        }
    }
    printf("alignment: phase %d, predicted = received %+lld, %.4f of table hits match on the search block\n", best_phase, (long long)best_delta, best);
    if (blocks > 0) {
        // Per block of frames at this alignment: how the read patterns relate to the predicted bits
        printf("block\texact\t|1|\t|2|\t|3|\tnone\thit%%\n");
        int64_t h[8] = {}, nb = 0, nh = 0; int64_t b0 = keys[0];
        auto flush = [&](int64_t bstart) {
            int64_t t = 0; for (int i = 0; i < 8; i++) t += h[i];
            if (t) printf("%lld\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\n", (long long)bstart, 100.0 * h[0] / t, 100.0 * h[1] / t, 100.0 * h[2] / t, 100.0 * h[3] / t, 100.0 * h[7] / t, 100.0 * nh / t);
            for (int i = 0; i < 8; i++) h[i] = 0; nh = 0;
        };
        uint8_t tg[588], kn[588];
        const EncodedFrames &PP = preds[best_phase];
        for (auto &[m, F] : frames) {
            if (m - b0 >= blocks) { flush(b0); b0 = m; }
            const int64_t g = m + best_delta;
            if (g < PP.first_valid || g >= (int64_t)PP.frames.size()) continue;
            frameBits(PP.frames[g], tg, kn);
            for (int s = 1; s <= 32; s++) {
                if (!F.seen[s]) continue;
                const int k = bestShift(F.s[s].efm, tg, kn, 27 + 17 * s); h[k == 99 ? 7 : abs(k)]++; nh += F.s[s].hit;
            }
        }
        flush(b0);
        return 0;
    }
    if (best < 0.3) { printf("no alignment: the decoder's symbol stream does not follow the prediction\n"); return 1; }
    const EncodedFrames &P = preds[best_phase];

    int64_t n = 0, hit = 0, hit_ok = 0, hit_wrong_ml_ok = 0, hit_wrong_ml_second = 0, hit_ok_ml_differs = 0;
    int64_t miss = 0, miss_ml_ok = 0, miss_ml_second = 0;
    vector<double> r_hit_ok, r_hit_wrong, r_miss_ok, r_miss_wrong;
    // Bit-level explanation of the wrong decisions: shift of the read pattern against the predicted bits
    int64_t sh_wrong_hit[8] = {}, sh_miss[8] = {}; // index: 0 = exact, 1..3 = |shift|, 7 = none
    uint8_t tog[588], known[588];
    for (auto &[m, F] : frames) {
        const int64_t g = m + best_delta;
        if (g < P.first_valid || g >= (int64_t)P.frames.size()) continue;
        frameBits(P.frames[g], tog, known);
        for (int s = 1; s <= 32; s++) {
            if (!F.seen[s]) continue;
            const Slot &S = F.s[s]; const uint8_t truth = P.frames[g][s - 1];
            const int k = bestShift(S.efm, tog, known, 27 + 17 * s);
            const int kk = k == 99 ? 7 : abs(k);
            if (S.hit && S.table != truth) sh_wrong_hit[kk]++;
            if (!S.hit) sh_miss[kk]++;
            const double ratio = S.p_best > 0 ? S.p_second / S.p_best : 1.0; // 1 = tie, 0 = decisive
            n++;
            if (S.hit) {
                hit++;
                if (S.table == truth) { hit_ok++; r_hit_ok.push_back(ratio); if (S.best != truth) hit_ok_ml_differs++; }
                else { r_hit_wrong.push_back(ratio); if (S.best == truth) hit_wrong_ml_ok++; else if (S.second == truth) hit_wrong_ml_second++; }
            } else {
                miss++;
                if (S.best == truth) { miss_ml_ok++; r_miss_ok.push_back(ratio); }
                else { r_miss_wrong.push_back(ratio); if (S.second == truth) miss_ml_second++; }
            }
        }
    }
    auto pc = [](int64_t a, int64_t b) { return b ? 100.0 * a / b : 0.0; };
    printf("symbols scored: %lld\n", (long long)n);
    printf("table hits:   %lld (%.2f%% of symbols); correct %.3f%%, wrong %.3f%%\n", (long long)hit, pc(hit, n), pc(hit_ok, hit), pc(hit - hit_ok, hit));
    printf("  of the wrong hits, the ML estimate has the truth as best: %.1f%%, as runner-up: %.1f%%\n", pc(hit_wrong_ml_ok, hit - hit_ok), pc(hit_wrong_ml_second, hit - hit_ok));
    printf("  of the correct hits, the ML estimate would have chosen differently: %.3f%%\n", pc(hit_ok_ml_differs, hit_ok));
    printf("table misses: %lld (%.2f%% of symbols); ML guess correct %.1f%%, truth is runner-up %.1f%%, neither %.1f%%\n",
           (long long)miss, pc(miss, n), pc(miss_ml_ok, miss), pc(miss_ml_second, miss), pc(miss - miss_ml_ok - miss_ml_second, miss));
    auto shifts = [&](const char *label, int64_t *h) {
        int64_t t = 0; for (int i = 0; i < 8; i++) t += h[i];
        printf("  %s: read pattern equals the predicted bits at shift 0: %.1f%%, |1| bit: %.1f%%, |2|: %.1f%%, |3|: %.1f%%, no shift fits (edge errors): %.1f%%\n",
               label, pc(h[0], t), pc(h[1], t), pc(h[2], t), pc(h[3], t), pc(h[7], t));
    };
    printf("bit-level explanation:\n"); shifts("wrong hits", sh_wrong_hit); shifts("misses    ", sh_miss);
    printf("ML score ratio runner-up/best (1 = tie, 0 = decisive):\n");
    percentiles(r_hit_ok, "hit, correct"); percentiles(r_hit_wrong, "hit, wrong");
    percentiles(r_miss_ok, "miss, guess correct"); percentiles(r_miss_wrong, "miss, guess wrong");
    // Erasure flag sweep: flag if ratio above a threshold
    vector<double> all_ok(r_hit_ok); all_ok.insert(all_ok.end(), r_miss_ok.begin(), r_miss_ok.end());
    vector<double> all_wrong(r_hit_wrong); all_wrong.insert(all_wrong.end(), r_miss_wrong.begin(), r_miss_wrong.end());
    if (!all_ok.empty() && !all_wrong.empty()) {
        printf("erasure flag on ratio > threshold (after taking the ML best as the symbol everywhere):\n");
        for (double thr : {0.9, 0.7, 0.5, 0.3, 0.1, 0.01}) {
            const double fw = 100.0 * count_if(all_wrong.begin(), all_wrong.end(), [&](double r) { return r > thr; }) / all_wrong.size();
            const double fo = 100.0 * count_if(all_ok.begin(), all_ok.end(), [&](double r) { return r > thr; }) / all_ok.size();
            printf("  thr %.2f: flags %.1f%% of wrong symbols and %.2f%% of correct ones\n", thr, fw, fo);
        }
    }
    return 0;
}
