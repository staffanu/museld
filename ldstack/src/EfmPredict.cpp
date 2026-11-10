// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)
//
// ldstack efmpredict: predict the EFM data symbols of a disc from its (known)
// audio and compare them with what a capture's EFM decoder received.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string>
#include <vector>
#include "CircEncoder.h"

using namespace std;

namespace {
    struct ReceivedFrame { uint8_t sym[32]; uint32_t erased; };

    // The "F:" lines of ac3rf-efm-decode --circ-debug-filename: 32 pairs of
    // "erased(0/1) hex"
    vector<ReceivedFrame> readCircDump(const string &path) {
        FILE *f = fopen(path.c_str(), "r");
        if (f == nullptr) throw runtime_error("cannot open " + path);
        vector<ReceivedFrame> out;
        char line[1024];
        while (fgets(line, sizeof line, f) != nullptr) {
            if (strncmp(line, "F:", 2) != 0) continue;
            ReceivedFrame r{};
            const char *p = line + 2;
            bool ok = true;
            for (int i = 0; i < 32; i++) {
                int e, v, n;
                if (sscanf(p, " %d %x%n", &e, &v, &n) != 2) { ok = false; break; }
                r.sym[i] = (uint8_t)v;
                if (e) r.erased |= 1u << i;
                p += n;
            }
            if (ok) out.push_back(r);
        }
        fclose(f);
        return out;
    }

    vector<int16_t> readPcm(const string &path) {
        FILE *f = fopen(path.c_str(), "rb");
        if (f == nullptr) throw runtime_error("cannot open " + path);
        vector<int16_t> v;
        int16_t buf[4096];
        size_t n;
        while ((n = fread(buf, sizeof(int16_t), 4096, f)) > 0) v.insert(v.end(), buf, buf + n);
        fclose(f);
        if (v.size() % 2) v.pop_back();
        return v;
    }

    // Matching non-erased symbols and the number compared, for received
    // frames [m0, m1) against predicted frames offset by delta
    pair<int64_t, int64_t> score(const vector<ReceivedFrame> &rec, const EncodedFrames &pred, int64_t delta,
                                 int64_t m0, int64_t m1) {
        int64_t match = 0, total = 0;
        for (int64_t m = m0; m < m1; m++) {
            const int64_t g = m + delta;
            if (g < pred.first_valid || g >= (int64_t)pred.frames.size()) continue;
            const auto &p = pred.frames[g];
            const auto &r = rec[m];
            for (int i = 0; i < 32; i++) {
                if (r.erased & (1u << i)) continue;
                total++;
                if (r.sym[i] == p[i]) match++;
            }
        }
        return {match, total};
    }
}

int cmdEfmPredict(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: ldstack efmpredict <audio.pcm (s16le stereo)> <circ-dump.txt> [--range MIN-MAX] [--report FILE]\n");
        return 1;
    }
    int64_t dmin = -4000, dmax = 4000;
    int local = 0, perframe = 0, only_phase = -1;
    string report;
    for (int i = 4; i < argc; i++) {
        const string a = argv[i];
        if (a == "--range" && i + 1 < argc) { long long x, y; if (sscanf(argv[++i], "%lld-%lld", &x, &y) == 2) { dmin = x; dmax = y; } }
        else if (a == "--report" && i + 1 < argc) report = argv[++i];
        else if (a == "--local" && i + 1 < argc) local = atoi(argv[++i]);
        else if (a == "--perframe" && i + 1 < argc) perframe = atoi(argv[++i]);
        else if (a == "--phase" && i + 1 < argc) only_phase = atoi(argv[++i]);
    }
    const auto pcm = readPcm(argv[2]);
    const auto rec = readCircDump(argv[3]);
    printf("%zu stereo samples, %zu received frames\n", pcm.size() / 2, rec.size());
    if (rec.size() < 1000) throw runtime_error("too few received frames");

    vector<EncodedFrames> preds;
    for (int phase = 0; phase < 6; phase++) preds.push_back(circEncode(pcm.data(), (int64_t)pcm.size() / 2, phase));
    if (perframe > 0) {
        // Every perframe-th received frame on its own: the predicted frame
        // (any phase, any offset in range) with the most matching symbols.
        // Chance gives at most 2-3 matches; a related frame gives many more.
        printf("received_frame\tphase\toffset\tmatches\tcompared\n");
        int hist[33] = {};
        for (int64_t m = 0; m < (int64_t)rec.size(); m += perframe) {
            int bm = -1, bp = 0; int64_t bd = 0, bt = 0;
            for (int phase = 0; phase < 6; phase++)
                for (int64_t delta = dmin; delta <= dmax; delta++) {
                    auto [match, total] = score(rec, preds[phase], delta, m, m + 1);
                    if (total > 0 && (int)match > bm) { bm = (int)match; bp = phase; bd = delta; bt = total; }
                }
            if (bm >= 0) { hist[min(32, bm)]++; if (bm >= 5) printf("%lld\t%d\t%+lld\t%d\t%lld\n", (long long)m, bp, (long long)bd, bm, (long long)bt); }
        }
        printf("histogram of best matches per frame:");
        for (int i = 0; i < 33; i++) if (hist[i]) printf(" %d:%d", i, hist[i]);
        printf("\n");
        return 0;
    }
    if (local > 0) {
        // Block-wise search: a damaged capture's decoder slips frames, so the
        // offset drifts; report the best (phase, offset) per block
        printf("block_start\tphase\toffset\tmatch\tcompared\terased%%\n");
        for (int64_t b = 0; b + local <= (int64_t)rec.size(); b += local) {
            double bb = -1; int bp = 0; int64_t bd = 0, bt = 0;
            for (int phase = 0; phase < 6; phase++)
                for (int64_t delta = dmin; delta <= dmax; delta++) {
                    auto [match, total] = score(rec, preds[phase], delta, b, b + local);
                    if (total > 100 && (double)match / total > bb) { bb = (double)match / total; bp = phase; bd = delta; bt = total; }
                }
            int64_t er = 0;
            for (int64_t m = b; m < b + local; m++) er += __builtin_popcount(rec[m].erased);
            printf("%lld\t%d\t%+lld\t%.4f\t%lld\t%.1f\n", (long long)b, bp, (long long)bd, bb, (long long)bt, 100.0 * er / (32.0 * local));
        }
        return 0;
    }
    // Coarse search on a block of 300 frames from the middle of the capture,
    // then refine the best (phase, offset) over everything
    const int64_t m0 = (int64_t)rec.size() / 2, m1 = min<int64_t>(rec.size(), m0 + 300);
    double best = -1; int best_phase = 0; int64_t best_delta = 0;
    for (int phase = 0; phase < 6; phase++) {
        for (int64_t delta = dmin; delta <= dmax; delta++) {
            auto [match, total] = score(rec, preds.back(), delta, m0, m1);
            if (total > 1000 && (double)match / total > best) { best = (double)match / total; best_phase = phase; best_delta = delta; }
        }
    }
    printf("best alignment: phase %d, predicted frame = received frame %+lld, match %.4f on the search block "
           "(chance level 0.0039)\n", best_phase, (long long)best_delta, best);
    const auto &pred = preds[best_phase];
    auto [match, total] = score(rec, pred, best_delta, 0, (int64_t)rec.size());
    int64_t erased = 0;
    for (auto &r : rec) erased += __builtin_popcount(r.erased);
    printf("whole capture: %lld of %lld non-erased symbols match (%.4f); erased symbols %lld of %zu (%.2f%%)\n",
           (long long)match, (long long)total, total ? (double)match / total : 0.0, (long long)erased, rec.size() * 32,
           100.0 * erased / (rec.size() * 32.0));
    if (!report.empty()) {
        FILE *f = fopen(report.c_str(), "w");
        if (f == nullptr) throw runtime_error("cannot write " + report);
        fprintf(f, "# received_frame\terased\twrong\tcompared\tpredicted_hex(32)\treceived_hex(32)\terased_mask\n");
        for (int64_t m = 0; m < (int64_t)rec.size(); m++) {
            const int64_t g = m + best_delta;
            if (g < pred.first_valid || g >= (int64_t)pred.frames.size()) continue;
            int wrong = 0, compared = 0;
            string ph, rh;
            for (int i = 0; i < 32; i++) {
                ph += format("{:02x}", pred.frames[g][i]);
                rh += format("{:02x}", rec[m].sym[i]);
                if (rec[m].erased & (1u << i)) continue;
                compared++;
                if (rec[m].sym[i] != pred.frames[g][i]) wrong++;
            }
            fprintf(f, "%lld\t%d\t%d\t%d\t%s\t%s\t%08x\n", (long long)m, __builtin_popcount(rec[m].erased), wrong, compared,
                    ph.c_str(), rh.c_str(), rec[m].erased);
        }
        fclose(f);
    }
    return 0;
}
