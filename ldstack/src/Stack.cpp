// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "Stack.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <map>
#include <stdexcept>
#include <unistd.h>
#include "Resample.h"
#include "FmDemod.h"

using namespace std;

namespace {
    struct RfInput {
        string name;
        int fd = -1;
        int lines_per_frame = 0, samples_per_line = 0;
        map<int64_t, int64_t> offsets; // picture key -> byte offset
        vector<float> frame;           // lines_per_frame * samples_per_line
        RfInput(const string &prefix) : name(prefix) {
            FILE *f = fopen((prefix + ".idx.tsv").c_str(), "r");
            if (f == nullptr) throw runtime_error("cannot open " + prefix + ".idx.tsv");
            char line[512];
            if (fgets(line, sizeof line, f) == nullptr
                || sscanf(line, "# lines_per_frame %d samples_per_line %d", &lines_per_frame, &samples_per_line) != 2)
                throw runtime_error("bad header in " + prefix + ".idx.tsv");
            while (fgets(line, sizeof line, f) != nullptr) {
                if (line[0] == '#') continue;
                int seq; long long key, k1, off; double start, spl;
                if (sscanf(line, "%d %lld %lld %lf %lf %lld", &seq, &key, &k1, &start, &spl, &off) == 6 && key >= 0)
                    offsets[key] = off;
            }
            fclose(f);
            fd = open((prefix + ".rf.i16").c_str(), O_RDONLY);
            if (fd < 0) throw runtime_error("cannot open " + prefix + ".rf.i16");
            frame.resize((size_t)lines_per_frame * samples_per_line);
        }
        ~RfInput() { if (fd >= 0) close(fd); }
        void read(int64_t key) {
            const size_t n = frame.size();
            vector<int16_t> raw(n);
            const int64_t off = offsets.at(key);
            size_t got = 0;
            while (got < n * sizeof(int16_t)) {
                const ssize_t r = pread(fd, (char *)raw.data() + got, n * sizeof(int16_t) - got, (off_t)(off + got));
                if (r <= 0) throw runtime_error("short read in " + name + ".rf.i16");
                got += (size_t)r;
            }
            for (size_t i = 0; i < n; i++) frame[i] = (float)raw[i];
        }
    };

    // Normalized cross-correlation of b against a at integer lag (b shifted
    // by lag: compares a[i] with b[i + lag]) over the interior of the line
    double ncc(const float *a, const float *b, int n, int lag, int margin) {
        double sab = 0, saa = 0, sbb = 0;
        for (int i = margin; i < n - margin; i++) {
            const double x = a[i], y = b[i + lag];
            sab += x * y; saa += x * x; sbb += y * y;
        }
        return saa > 0 && sbb > 0 ? sab / sqrt(saa * sbb) : 0;
    }
}

void stackCaptures(const string &out, const vector<string> &inputs, const StackOptions &opt) {
    if (inputs.size() < 2) throw runtime_error("stack needs at least two inputs");
    vector<unique_ptr<RfInput>> in;
    for (auto &p : inputs) in.push_back(make_unique<RfInput>(p));
    const int L = in[0]->lines_per_frame, N = in[0]->samples_per_line;
    for (auto &i : in)
        if (i->lines_per_frame != L || i->samples_per_line != N)
            throw runtime_error("inputs differ in lines per frame or samples per line");
    // Common pictures, in the reference's order
    vector<int64_t> keys;
    for (auto &[key, off] : in[0]->offsets) {
        bool all = true;
        for (size_t i = 1; i < in.size(); i++) if (!in[i]->offsets.count(key)) all = false;
        if (all) keys.push_back(key);
    }
    if (keys.empty()) throw runtime_error("no pictures common to all inputs");

    FILE *rf = fopen((out + ".rf.i16").c_str(), "wb");
    FILE *idx = fopen((out + ".idx.tsv").c_str(), "w");
    FILE *st = fopen((out + ".stack.f64").c_str(), "wb");
    if (rf == nullptr || idx == nullptr || st == nullptr) throw runtime_error("cannot write " + out + ".*");
    fprintf(idx, "# lines_per_frame %d samples_per_line %d\n# seq\tpicture_key\tk_line1\tstart\tsamples_per_line\tbyte_offset\n", L, N);

    Resampler rs;
    const int n_in = (int)in.size();
    const int margin = opt.max_lag + 8;
    const int n_win = (N + opt.window - 1) / opt.window;
    vector<vector<float>> aligned(n_in, vector<float>(N));
    vector<vector<float>> win_rms(n_in, vector<float>(n_win));
    vector<float> acc(N), wsum(N);
    vector<int16_t> line_out(N);
    vector<double> rec(2 + 4 * n_in);
    // Totals for the summary
    vector<double> sum_cc(n_in, 0), sum_drop(n_in, 0);
    vector<int64_t> n_lines_used(n_in, 0), n_lines_rejected(n_in, 0);
    int64_t byte_offset = 0, total_lines = 0;

    for (size_t ki = 0; ki < keys.size(); ki++) {
        const int64_t key = keys[ki];
        for (auto &i : in) i->read(key);
        fprintf(idx, "%d\t%lld\t0\t0\t0\t%lld\n", (int)ki, (long long)key, (long long)byte_offset);
        for (int l = 0; l < L; l++) {
            const float *ref = in[0]->frame.data() + (size_t)l * N;
            rec[0] = (double)ki; rec[1] = l;
            vector<bool> usable(n_in, true);
            // Reference: as is
            copy(ref, ref + N, aligned[0].begin());
            rec[2] = 0; rec[3] = 1; rec[4] = 1;
            // The reference line may itself be empty (missing line): then the
            // others cannot be aligned to it
            double ref_energy = 0;
            for (int i = 0; i < N; i++) ref_energy += (double)ref[i] * ref[i];
            const bool ref_ok = ref_energy > 0;
            for (int c = 1; c < n_in; c++) {
                const float *b = in[c]->frame.data() + (size_t)l * N;
                double best_cc = -2; int best_lag = 0;
                if (ref_ok)
                    for (int lag = -opt.max_lag; lag <= opt.max_lag; lag++) {
                        const double v = ncc(ref, b, N, lag, margin);
                        if (v > best_cc) { best_cc = v; best_lag = lag; }
                    }
                double lag = best_lag;
                if (ref_ok && abs(best_lag) < opt.max_lag) {
                    // Parabolic refinement
                    const double cm = ncc(ref, b, N, best_lag - 1, margin), cp = ncc(ref, b, N, best_lag + 1, margin);
                    const double den = cm - 2 * best_cc + cp;
                    if (den < 0) lag += 0.5 * (cm - cp) / den;
                }
                rs.shift(b, N, lag, aligned[c].data());
                // Gain match on the aligned line
                double sab = 0, sbb = 0;
                for (int i = margin; i < N - margin; i++) { sab += (double)ref[i] * aligned[c][i]; sbb += (double)aligned[c][i] * aligned[c][i]; }
                double gain = sbb > 0 ? sab / sbb : 1;
                gain = clamp(gain, 0.5, 2.0);
                for (int i = 0; i < N; i++) aligned[c][i] *= (float)gain;
                usable[c] = ref_ok && best_cc >= opt.min_cc;
                rec[2 + 4 * c] = lag; rec[3 + 4 * c] = best_cc; rec[4 + 4 * c] = gain;
                if (usable[c]) { sum_cc[c] += best_cc; n_lines_used[c]++; } else n_lines_rejected[c]++;
            }
            // Dropout windows: a capture whose window RMS is well below the
            // best capture's is left out of that window
            for (int c = 0; c < n_in; c++)
                for (int w = 0; w < n_win; w++) {
                    double s = 0; int n = 0;
                    for (int i = w * opt.window; i < min(N, (w + 1) * opt.window); i++, n++) s += (double)aligned[c][i] * aligned[c][i];
                    win_rms[c][w] = (float)sqrt(s / max(1, n));
                }
            fill(acc.begin(), acc.end(), 0.0f);
            fill(wsum.begin(), wsum.end(), 0.0f);
            vector<int> drop_count(n_in, 0);
            for (int w = 0; w < n_win; w++) {
                float best = 0;
                for (int c = 0; c < n_in; c++) if (usable[c]) best = max(best, win_rms[c][w]);
                for (int c = 0; c < n_in; c++) {
                    if (!usable[c]) continue;
                    const bool drop = win_rms[c][w] < opt.dropout_ratio * best;
                    if (drop) { drop_count[c]++; continue; }
                    for (int i = w * opt.window; i < min(N, (w + 1) * opt.window); i++) { acc[i] += aligned[c][i]; wsum[i] += 1; }
                }
            }
            for (int i = 0; i < N; i++) {
                const float v = wsum[i] > 0 ? acc[i] / wsum[i] : ref[i];
                line_out[i] = (int16_t)max(-32768.0f, min(32767.0f, roundf(v)));
            }
            for (int c = 0; c < n_in; c++) {
                rec[5 + 4 * c] = usable[c] ? (double)drop_count[c] / n_win : 1.0;
                if (usable[c]) sum_drop[c] += rec[5 + 4 * c];
            }
            fwrite(line_out.data(), sizeof(int16_t), N, rf);
            fwrite(rec.data(), sizeof(double), rec.size(), st);
            total_lines++;
        }
        byte_offset += (int64_t)L * N * sizeof(int16_t);
        if (opt.verbose && ki % 100 == 0) fprintf(stderr, "\r  %zu / %zu pictures", ki, keys.size());
    }
    if (opt.verbose) fprintf(stderr, "\n");
    fclose(rf); fclose(idx); fclose(st);
    printf("%zu pictures stacked from %d captures\n", keys.size(), n_in);
    for (int c = 0; c < n_in; c++) {
        if (c == 0) printf("  %s: reference, dropout windows %.3f%%\n", in[c]->name.c_str(), 100.0 * sum_drop[c] / max<int64_t>(1, total_lines));
        else printf("  %s: mean cc %.3f, lines rejected %lld of %lld, dropout windows %.3f%%\n", in[c]->name.c_str(),
                    sum_cc[c] / max<int64_t>(1, n_lines_used[c]), (long long)n_lines_rejected[c], (long long)total_lines,
                    100.0 * sum_drop[c] / max<int64_t>(1, n_lines_used[c]));
    }
}

void stackBaseband(const string &out, const vector<string> &inputs, double carrier_hz, const StackOptions &opt) {
    if (inputs.empty()) throw runtime_error("stackbb needs at least one input");
    vector<unique_ptr<RfInput>> in;
    for (auto &p : inputs) in.push_back(make_unique<RfInput>(p));
    const int L = in[0]->lines_per_frame, N = in[0]->samples_per_line;
    for (auto &i : in)
        if (i->lines_per_frame != L || i->samples_per_line != N)
            throw runtime_error("inputs differ in lines per frame or samples per line");
    vector<int64_t> keys;
    for (auto &[key, off] : in[0]->offsets) {
        bool all = true;
        for (size_t i = 1; i < in.size(); i++) if (!in[i]->offsets.count(key)) all = false;
        if (all) keys.push_back(key);
    }
    if (keys.empty()) throw runtime_error("no pictures common to all inputs");

    // The line-locked grid is 2560 samples per 64 us line (or 910 * ... for
    // NTSC), i.e. the nominal 40 MHz; the demodulator runs at that rate
    constexpr int c_decim = 2;
    const double rf_rate = N / 64.0e-6; // samples per second on the line-locked grid
    const int M = N / c_decim;          // baseband samples per line
    FILE *bb = fopen((out + ".bb.f32").c_str(), "wb");
    FILE *idx = fopen((out + ".idx.tsv").c_str(), "w");
    FILE *st = fopen((out + ".stack.f64").c_str(), "wb");
    if (bb == nullptr || idx == nullptr || st == nullptr) throw runtime_error("cannot write " + out + ".*");
    fprintf(idx, "# lines_per_frame %d samples_per_line %d\n# seq\tpicture_key\tk_line1\tstart\tsamples_per_line\tbyte_offset\n", L, M);

    Resampler rs;
    const int n_in = (int)in.size();
    const int margin = opt.max_lag + 8;
    const int win = max(8, opt.window / c_decim);
    const int n_win = (M + win - 1) / win;
    vector<vector<float>> demod(n_in), aligned(n_in, vector<float>(M));
    vector<vector<float>> win_rms(n_in, vector<float>(n_win));
    vector<float> acc(M), wsum(M), line_out(M);
    vector<double> rec(2 + 3 * n_in);
    vector<double> sum_cc(n_in, 0), sum_drop(n_in, 0);
    vector<int64_t> n_used(n_in, 0), n_rej(n_in, 0);
    int64_t byte_offset = 0, total_lines = 0;
    // Demodulator group delay in baseband samples, rounded
    const int delay_out = (int)llround(FmDemod(rf_rate, carrier_hz, c_decim, 2.2e6).delay() / c_decim);

    for (size_t ki = 0; ki < keys.size(); ki++) {
        const int64_t key = keys[ki];
        for (int c = 0; c < n_in; c++) {
            in[c]->read(key);
            // Demodulate the whole frame as one stream (the lines are
            // contiguous in time), then drop the group delay
            FmDemod fm(rf_rate, carrier_hz, c_decim, 2.2e6);
            demod[c].clear();
            fm.process(in[c]->frame.data(), (int64_t)L * N, demod[c]);
            demod[c].erase(demod[c].begin(), demod[c].begin() + min<size_t>(demod[c].size(), (size_t)delay_out));
            demod[c].resize((size_t)L * M, demod[c].empty() ? 0.0f : demod[c].back());
        }
        fprintf(idx, "%d\t%lld\t0\t0\t0\t%lld\n", (int)ki, (long long)key, (long long)byte_offset);
        for (int l = 0; l < L; l++) {
            const float *ref = demod[0].data() + (size_t)l * M;
            rec[0] = (double)ki; rec[1] = l;
            vector<bool> usable(n_in, true);
            copy(ref, ref + M, aligned[0].begin());
            rec[2] = 0; rec[3] = 1;
            for (int c = 1; c < n_in; c++) {
                const float *b = demod[c].data() + (size_t)l * M;
                // Correlate the frequency deviation from the carrier, so the
                // constant carrier does not dominate
                vector<float> ra(M), rb(M);
                for (int i = 0; i < M; i++) { ra[i] = ref[i] - (float)carrier_hz; rb[i] = b[i] - (float)carrier_hz; }
                double best_cc = -2; int best_lag = 0;
                for (int lag = -opt.max_lag; lag <= opt.max_lag; lag++) {
                    const double v = ncc(ra.data(), rb.data(), M, lag, margin);
                    if (v > best_cc) { best_cc = v; best_lag = lag; }
                }
                double lag = best_lag;
                if (abs(best_lag) < opt.max_lag) {
                    const double cm = ncc(ra.data(), rb.data(), M, best_lag - 1, margin), cp = ncc(ra.data(), rb.data(), M, best_lag + 1, margin);
                    const double den = cm - 2 * best_cc + cp;
                    if (den < 0) lag += 0.5 * (cm - cp) / den;
                }
                rs.shift(b, M, lag, aligned[c].data());
                usable[c] = best_cc >= opt.min_cc;
                rec[2 + 3 * c] = lag; rec[3 + 3 * c] = best_cc;
                if (usable[c]) { sum_cc[c] += best_cc; n_used[c]++; } else n_rej[c]++;
            }
            // Dropout windows from the RF envelope of each capture's line
            for (int c = 0; c < n_in; c++) {
                const float *rfl = in[c]->frame.data() + (size_t)l * N;
                for (int w = 0; w < n_win; w++) {
                    double s = 0; int n = 0;
                    for (int i = w * win * c_decim; i < min(N, (w + 1) * win * c_decim); i++, n++) s += (double)rfl[i] * rfl[i];
                    win_rms[c][w] = (float)sqrt(s / max(1, n));
                }
            }
            fill(acc.begin(), acc.end(), 0.0f);
            fill(wsum.begin(), wsum.end(), 0.0f);
            vector<int> drop_count(n_in, 0);
            for (int w = 0; w < n_win; w++) {
                float best = 0;
                for (int c = 0; c < n_in; c++) if (usable[c]) best = max(best, win_rms[c][w]);
                for (int c = 0; c < n_in; c++) {
                    if (!usable[c]) continue;
                    if (win_rms[c][w] < opt.dropout_ratio * best) { drop_count[c]++; continue; }
                    for (int i = w * win; i < min(M, (w + 1) * win); i++) { acc[i] += aligned[c][i]; wsum[i] += 1; }
                }
            }
            for (int i = 0; i < M; i++) line_out[i] = wsum[i] > 0 ? acc[i] / wsum[i] : ref[i];
            for (int c = 0; c < n_in; c++) {
                rec[4 + 3 * c] = usable[c] ? (double)drop_count[c] / n_win : 1.0;
                if (usable[c]) sum_drop[c] += rec[4 + 3 * c];
            }
            fwrite(line_out.data(), sizeof(float), M, bb);
            fwrite(rec.data(), sizeof(double), rec.size(), st);
            total_lines++;
        }
        byte_offset += (int64_t)L * M * sizeof(float);
        if (opt.verbose && ki % 100 == 0) fprintf(stderr, "\r  %zu / %zu pictures", ki, keys.size());
    }
    if (opt.verbose) fprintf(stderr, "\n");
    fclose(bb); fclose(idx); fclose(st);
    printf("%zu pictures demodulated and stacked from %d captures\n", keys.size(), n_in);
    for (int c = 0; c < n_in; c++) {
        if (c == 0) printf("  %s: reference, dropout windows %.3f%%\n", in[c]->name.c_str(), 100.0 * sum_drop[c] / max<int64_t>(1, total_lines));
        else printf("  %s: mean baseband cc %.3f, lines rejected %lld of %lld, dropout windows %.3f%%\n", in[c]->name.c_str(),
                    sum_cc[c] / max<int64_t>(1, n_used[c]), (long long)n_rej[c], (long long)total_lines,
                    100.0 * sum_drop[c] / max<int64_t>(1, n_used[c]));
    }
}
