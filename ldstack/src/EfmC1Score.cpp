// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)
//
// ldstack efmc1: score the C1 stage of ac3rf-efm-decode against the truth.
// Reads the --circ-debug-filename dump (F: received frame, C1: the C1 input
// word, D1: the word after C1 decoding) and the audio, predicts the C1 words,
// and classifies every C1 output as correct, erased (some symbols left
// flagged), or miscorrected (an unflagged symbol that is wrong).

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "CircEncoder.h"

using namespace std;

namespace {
    struct Word { uint8_t sym[32]; uint32_t erased; bool ok; };
    bool parseWord(const char *p, Word &w) {
        w.erased = 0;
        for (int i = 0; i < 32; i++) {
            int e, v, n;
            if (sscanf(p, " %d %x%n", &e, &v, &n) != 2) return false;
            w.sym[i] = (uint8_t)v; if (e) w.erased |= 1u << i; p += n;
        }
        return true;
    }
    vector<int16_t> readPcm(const string &path) {
        FILE *f = fopen(path.c_str(), "rb");
        if (f == nullptr) throw runtime_error("cannot open " + path);
        vector<int16_t> v; int16_t buf[4096]; size_t n;
        while ((n = fread(buf, sizeof(int16_t), 4096, f)) > 0) v.insert(v.end(), buf, buf + n);
        fclose(f);
        if (v.size() % 2) v.pop_back();
        return v;
    }
}

int cmdEfmC1(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: ldstack efmc1 <audio.pcm> <circ-dump.txt> [--phase P] [--range MIN-MAX]\n"); return 1; }
    int only_phase = -1; int64_t dmin = -3000, dmax = 3000;
    for (int i = 4; i < argc; i++) {
        const string a = argv[i];
        if (a == "--phase" && i + 1 < argc) only_phase = atoi(argv[++i]);
        else if (a == "--range" && i + 1 < argc) { long long x, y; if (sscanf(argv[++i], "%lld-%lld", &x, &y) == 2) { dmin = x; dmax = y; } }
    }
    // Frames in order: F, C1, D1 per frame
    vector<Word> F, C1, D1;
    {
        FILE *f = fopen(argv[3], "r");
        if (f == nullptr) throw runtime_error("cannot open dump");
        char line[2048]; Word w;
        while (fgets(line, sizeof line, f) != nullptr) {
            if (!strncmp(line, "F:", 2) && parseWord(line + 2, w)) F.push_back(w);
            else if (!strncmp(line, "C1:", 3) && parseWord(line + 3, w)) C1.push_back(w);
            else if (!strncmp(line, "D1:", 3) && parseWord(line + 3, w)) D1.push_back(w);
        }
        fclose(f);
    }
    const size_t nf = min(F.size(), min(C1.size(), D1.size()));
    printf("%zu frames with F/C1/D1 records\n", nf);
    const auto pcm = readPcm(argv[2]);
    vector<EncodedFrames> preds;
    for (int phase = 0; phase < 6; phase++) preds.push_back(circEncode(pcm.data(), (int64_t)pcm.size() / 2, phase));
    // Alignment on the received frames' non-erased symbols, per block of 300 frames
    auto score = [&](const EncodedFrames &p, int64_t delta, size_t m0, size_t m1) {
        int64_t match = 0, total = 0;
        for (size_t m = m0; m < m1; m++) {
            const int64_t g = (int64_t)m + delta;
            if (g < p.first_valid || g >= (int64_t)p.frames.size()) continue;
            for (int i = 0; i < 32; i++) {
                if (F[m].erased & (1u << i)) continue;
                total++; if (F[m].sym[i] == p.frames[g][i]) match++;
            }
        }
        return make_pair(match, total);
    };
    const size_t m0 = nf / 2, m1 = min(nf, m0 + 300);
    double best = -1; int bp = 0; int64_t bd = 0;
    for (int phase = 0; phase < 6; phase++) {
        if (only_phase >= 0 && phase != only_phase) continue;
        for (int64_t d = dmin; d <= dmax; d++) {
            auto [mm, tt] = score(preds[phase], d, m0, m1);
            if (tt > 500 && (double)mm / tt > best) { best = (double)mm / tt; bp = phase; bd = d; }
        }
    }
    printf("alignment: phase %d, offset %+lld, match %.3f\n", bp, (long long)bd, best);
    if (best < 0.3) { printf("no alignment\n"); return 1; }
    const EncodedFrames &P = preds[bp];
    // The C1 word of frame m holds symbol i from received frame m - (i % 2), parity un-inverted
    int64_t n = 0, in_ok = 0, out_correct = 0, out_erased = 0, out_miscorrected = 0, in_err_hist[8] = {};
    int64_t mis_by_in[8] = {};
    for (size_t b = 0; b < nf; b += 300) {
        double bb = -1; int64_t cur = bd;
        for (int64_t d = bd - 40; d <= bd + 40; d++) {
            auto [mm, tt] = score(P, d, b, min(nf, b + 300));
            if (tt > 50 && (double)mm / tt > bb) { bb = (double)mm / tt; cur = d; }
        }
        if (bb < 0.15) continue;
        for (size_t m = b; m < min(nf, b + 300); m++) {
            if (m < 1) continue;
            const int64_t g = (int64_t)m + cur;
            if (g - 1 < P.first_valid || g >= (int64_t)P.frames.size()) continue;
            uint8_t truth[32];
            for (int i = 0; i < 32; i++) {
                uint8_t v = P.frames[g - (i % 2)][i];
                if ((i >= 12 && i < 16) || i >= 28) v = (uint8_t)~v;
                truth[i] = v;
            }
            int errs = 0; for (int i = 0; i < 32; i++) if (!(C1[m].erased & (1u << i)) && C1[m].sym[i] != truth[i]) errs++;
            const int eras = __builtin_popcount(C1[m].erased);
            n++; in_err_hist[min(7, errs)]++;
            if (errs == 0 && eras == 0) in_ok++;
            bool any_erased = D1[m].erased != 0, wrong = false;
            for (int i = 0; i < 32; i++) if (!(D1[m].erased & (1u << i)) && D1[m].sym[i] != truth[i]) wrong = true;
            if (wrong) { out_miscorrected++; mis_by_in[min(7, errs)]++; }
            else if (any_erased) out_erased++;
            else out_correct++;
        }
    }
    auto pc = [&](int64_t a) { return n ? 100.0 * a / n : 0.0; };
    printf("C1 words scored: %lld; clean at input %.2f%%\n", (long long)n, pc(in_ok));
    printf("input errors per word (unflagged wrong symbols): 0: %.1f%%  1: %.1f%%  2: %.1f%%  3: %.1f%%  4+: %.1f%%\n",
           pc(in_err_hist[0]), pc(in_err_hist[1]), pc(in_err_hist[2]), pc(in_err_hist[3]), pc(in_err_hist[4] + in_err_hist[5] + in_err_hist[6] + in_err_hist[7]));
    printf("C1 output: correct %.2f%%, erased %.2f%%, MISCORRECTED (unflagged wrong symbol) %.3f%%\n", pc(out_correct), pc(out_erased), pc(out_miscorrected));
    printf("  miscorrections by input errors: 0: %lld  1: %lld  2: %lld  3: %lld  4+: %lld\n", (long long)mis_by_in[0], (long long)mis_by_in[1],
           (long long)mis_by_in[2], (long long)mis_by_in[3], (long long)(mis_by_in[4] + mis_by_in[5] + mis_by_in[6] + mis_by_in[7]));
    return 0;
}
