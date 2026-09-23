// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "Scanner.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <format>
#include <limits>
#include <map>
#include <stdexcept>
#include "FmDemod.h"
#include "LdsFile.h"

using namespace std;

namespace {
    constexpr double c_nan = numeric_limits<double>::quiet_NaN();

    struct Pulse {
        double fall;   // input samples
        double width;  // microseconds
    };
    enum class PulseClass { eJunk, eEqualizing, eHsync, eBroad };
    PulseClass classify(double width_us) {
        if (width_us >= 3.0 && width_us <= 6.5) return PulseClass::eHsync;
        if (width_us >= 20.0 && width_us <= 35.0) return PulseClass::eBroad;
        if (width_us >= 1.5 && width_us < 3.0) return PulseClass::eEqualizing;
        return PulseClass::eJunk;
    }

    // A vertical sync group as seen in pass A, with the VBI code words read
    // from the lines that follow it
    struct FieldEvent {
        double broad_start;  // input samples, fall of the first broad pulse
        bool field1;         // the group starts on a line boundary
        vector<int> codes;
    };

    // Least-squares quadratic through (x, y) evaluated at x0; falls back to a
    // line or a constant with few points
    double localFit(const vector<pair<double, double>> &pts, double x0) {
        const size_t n = pts.size();
        if (n == 0) return c_nan;
        if (n < 3) {
            if (n == 1) return pts[0].second;
            const double slope = (pts[1].second - pts[0].second) / (pts[1].first - pts[0].first);
            return pts[0].second + slope * (x0 - pts[0].first);
        }
        // Center on x0 for conditioning; y is offset by the first value
        double s[5] = {}, b[3] = {};
        const double y0 = pts[0].second;
        for (auto &[x, y] : pts) {
            const double dx = x - x0, dy = y - y0;
            double p = 1;
            for (int i = 0; i < 5; i++, p *= dx) {
                s[i] += p;
                if (i < 3) b[i] += p * dy;
            }
        }
        const bool quadratic = n >= 5;
        if (!quadratic) {
            const double det = s[0] * s[2] - s[1] * s[1];
            return y0 + (b[0] * s[2] - b[1] * s[1]) / det;
        }
        // Solve the 3x3 normal equations for [c0, c1, c2]; c0 is the value at x0
        double m[3][4] = {{s[0], s[1], s[2], b[0]}, {s[1], s[2], s[3], b[1]}, {s[2], s[3], s[4], b[2]}};
        for (int c = 0; c < 3; c++) {
            int piv = c;
            for (int r = c + 1; r < 3; r++)
                if (fabs(m[r][c]) > fabs(m[piv][c])) piv = r;
            swap(m[c], m[piv]);
            for (int r = 0; r < 3; r++) {
                if (r == c) continue;
                const double f = m[r][c] / m[c][c];
                for (int k = c; k < 4; k++) m[r][k] -= f * m[c][k];
            }
        }
        return y0 + m[0][3] / m[0][0];
    }
}

vector<FrameRecord> scanCapture(const string &lds_path, const ScanOptions &opt) {
    LdsFile file(lds_path);
    const int64_t total = opt.max_samples > 0 ? min(opt.max_samples, file.sampleCount()) : file.sampleCount();

    constexpr int c_decim = 4;
    FmDemod demod(opt.sample_rate, opt.carrier_hz, c_decim, 2.2e6);
    const double out_rate = demod.outputRate();
    const double samples_per_us = out_rate / 1e6;
    const double line_samples_out = opt.line_us * samples_per_us; // demodulated samples per line
    const double line_samples_in = opt.line_us * opt.sample_rate / 1e6;

    // --- pass A: pulses, vertical groups and VBI codes, streaming ---
    vector<Pulse> pulses;
    vector<FieldEvent> fields;

    // Demodulated signal ring (the VBI lines are read back from it a few
    // lines after their vertical group)
    constexpr int64_t c_ring = 1 << 21; // 0.2 s at 10 MHz
    vector<float> ring(c_ring, 0.0f), smooth_ring(c_ring, 0.0f);
    int64_t produced = 0; // demodulated samples so far (index of the next one)

    // Levels: sync tip and blanking, from the frequency histogram of the
    // first stretch; then fixed
    double level_sync = c_nan, level_blank = c_nan;
    auto estimateLevels = [&](const vector<float> &f) {
        const double lo = opt.carrier_hz - 1.5e6, hi = opt.carrier_hz + 1.5e6, bin = 25e3;
        const int nb = (int)((hi - lo) / bin);
        vector<int64_t> h(nb, 0);
        for (float v : f) {
            const int b = (int)((v - lo) / bin);
            if (b >= 0 && b < nb) h[b]++;
        }
        // The sync tip is the lowest strong peak: at least 2% of the samples
        // sit at it (5 us of every 64); the blanking level the strongest peak
        // 0.2 - 0.6 MHz above it
        const int64_t floor_count = (int64_t)(0.01 * f.size());
        int b_sync = -1;
        for (int b = 1; b + 1 < nb; b++)
            if (h[b] > floor_count && h[b] >= h[b - 1] && h[b] >= h[b + 1]) { b_sync = b; break; }
        if (b_sync < 0) throw runtime_error("no sync tip level found in the frequency histogram");
        int b_blank = -1;
        for (int b = b_sync + (int)(0.2e6 / bin); b < min(nb, b_sync + (int)(0.6e6 / bin)); b++)
            if (b_blank < 0 || h[b] > h[b_blank]) b_blank = b;
        level_sync = lo + (b_sync + 0.5) * bin;
        level_blank = lo + (b_blank + 0.5) * bin;
    };

    // Hysteresis slicer state
    bool below = false;
    double fall_pos = 0; // demodulated sample index of the pending fall
    double thr_mid = 0, thr_lo = 0, thr_hi = 0;

    // Vertical group detection
    double last_broad = -1e18, last_hsync = -1e18;
    double period_est = line_samples_out; // demodulated samples, tracked from consecutive hsyncs
    deque<pair<size_t, double>> pending_vbi; // (field index, first VBI line start, demod samples)

    // A demodulated sample m was computed from input samples centred on
    // c_decim * m - delay (the lowpass output lags its input); the slicer
    // has already taken out the boxcar's delay
    auto demodToInput = [&](double m) { return m * c_decim - demod.delay(); };

    vector<float> chunk_out;
    constexpr int64_t c_chunk = 1 << 20;
    vector<float> first_stretch;
    bool levels_set = false;
    for (int64_t pos = 0; pos < total; pos += c_chunk) {
        const int64_t n = min(c_chunk, total - pos);
        const float *in = file.samples(pos, n);
        chunk_out.clear();
        demod.process(in, n, chunk_out);
        if (!levels_set) {
            first_stretch.insert(first_stretch.end(), chunk_out.begin(), chunk_out.end());
            if (first_stretch.size() < (size_t)(out_rate * 0.2) && pos + n < total)
                { /* keep collecting, but still store into the ring below */ }
            else {
                estimateLevels(first_stretch);
                thr_mid = (level_sync + level_blank) / 2;
                thr_lo = thr_mid - 0.04e6;
                thr_hi = thr_mid + 0.04e6;
                levels_set = true;
                if (opt.verbose)
                    fprintf(stderr, "levels: sync tip %.3f MHz, blanking %.3f MHz\n", level_sync / 1e6, level_blank / 1e6);
                // Slice what was collected so far by re-running the slicer
                // over the ring: simplest is to reprocess first_stretch
                chunk_out.swap(first_stretch);
                produced = 0;
            }
        }
        if (!levels_set) {
            // Not sliced yet; nothing else to do until the levels are known
            continue;
        }
        for (float v : chunk_out) {
            const int64_t m = produced++;
            ring[m & (c_ring - 1)] = v;
            // 5-sample boxcar for slicing (0.5 us), delayed by 2 samples
            float s = 0;
            for (int j = 0; j < 5; j++) s += ring[(m - j) & (c_ring - 1)];
            s *= 0.2f;
            const int64_t ms = m - 2;
            smooth_ring[ms & (c_ring - 1)] = s;
            const float s_prev = smooth_ring[(ms - 1) & (c_ring - 1)];
            if (!below && s < thr_lo) {
                below = true;
                // Interpolated crossing of the mid threshold: the mid level is
                // between s_prev (above) and s only if the drop was steep; use
                // the low-threshold crossing otherwise
                const float a = s_prev, b = s;
                double frac = a > thr_lo && a != b ? (a - thr_lo) / (a - b) : 0.5;
                fall_pos = (double)(ms - 1) + frac;
            } else if (below && s > thr_hi) {
                below = false;
                const float a = s_prev, b = s;
                double frac = a < thr_hi && a != b ? (thr_hi - a) / (b - a) : 0.5;
                const double rise_pos = (double)(ms - 1) + frac;
                const double width_us = (rise_pos - fall_pos) / samples_per_us;
                const PulseClass cls = classify(width_us);
                if (cls == PulseClass::eJunk) continue;
                pulses.push_back({demodToInput(fall_pos), width_us});
                if (cls == PulseClass::eHsync) {
                    const double dt = fall_pos - last_hsync;
                    if (dt > 0.9 * period_est && dt < 1.1 * period_est)
                        period_est += 0.02 * (dt - period_est);
                    last_hsync = fall_pos;
                } else if (cls == PulseClass::eBroad) {
                    if (fall_pos - last_broad > 5 * period_est) {
                        // Start of a vertical group.  Field 1 when it lies a
                        // whole number of lines after the last hsync pulse
                        const double phase = (fall_pos - last_hsync) / period_est;
                        const double frac = phase - floor(phase);
                        const bool field1 = frac < 0.25 || frac > 0.75;
                        fields.push_back({demodToInput(fall_pos), field1, {}});
                        // VBI lines 16-18 of the field start 15 (field 1) or
                        // 15.5 (field 2) lines after the group start
                        pending_vbi.emplace_back(fields.size() - 1, fall_pos + (field1 ? 15.0 : 15.5) * period_est);
                    }
                    last_broad = fall_pos;
                }
            }
            // Decode pending VBI lines once their samples are in (three lines
            // plus a margin past the first)
            while (!pending_vbi.empty() && pending_vbi.front().second + 4 * period_est < (double)m) {
                auto [fi, start] = pending_vbi.front();
                pending_vbi.pop_front();
                for (int l = 0; l < 3; l++) {
                    double s0 = start + l * period_est;
                    // Snap to the nearest hsync pulse fall, if one is within 2 us
                    // (search the recent pulses)
                    for (size_t i = pulses.size(); i-- > 0;) {
                        const double p = (pulses[i].fall + demod.delay()) / c_decim;
                        if (p < s0 - 3 * period_est) break;
                        if (fabs(p - s0) < 2 * samples_per_us) { s0 = p; break; }
                    }
                    // Copy the line out of the ring (it may wrap)
                    const int64_t i0 = (int64_t)s0;
                    if (i0 < m - c_ring + 8 || i0 + (int64_t)line_samples_out + 8 > m) continue;
                    vector<float> line((size_t)line_samples_out + 8);
                    for (size_t j = 0; j < line.size(); j++) line[j] = ring[(i0 + j) & (c_ring - 1)];
                    const int code = decodePhilipsLine(line.data(), line_samples_out, level_blank,
                                                       level_blank + opt.white_hz_offset);
                    if (code >= 0) fields[fi].codes.push_back(code);
                }
            }
        }
        if (opt.verbose && (pos / c_chunk) % 400 == 0)
            fprintf(stderr, "\r  scanned %.1f s", pos / opt.sample_rate);
    }
    if (opt.verbose)
        fprintf(stderr, "\r  scanned %.1f s: %zu pulses, %zu vertical groups\n", total / opt.sample_rate, pulses.size(), fields.size());

    // --- pass B: lattice ---
    // Chain the hsync pulses: the period is known to ~0.1 %, so the number of
    // lines between two pulses is unambiguous for gaps of many lines
    struct Meas { int64_t k; double t; };
    vector<Meas> meas;
    double p_run = line_samples_in;
    {
        double last_t = c_nan;
        int64_t k = 0;
        for (const auto &p : pulses) {
            if (classify(p.width) != PulseClass::eHsync) continue;
            if (isnan(last_t)) { last_t = p.fall; meas.push_back({0, p.fall}); continue; }
            const double dt = p.fall - last_t;
            const double dk_f = dt / p_run;
            const int64_t dk = llround(dk_f);
            if (dk < 1) continue; // a second pulse on the same line: junk
            const double tol = (0.5 + 0.5 * (double)dk) * opt.sample_rate / 1e6; // 0.5 us per line of gap
            if (fabs(dt - dk * p_run) > tol) continue;
            k += dk;
            if (dk == 1) p_run += 0.01 * (dt - p_run);
            last_t = p.fall;
            meas.push_back({k, p.fall});
        }
    }
    if (meas.size() < 1000)
        throw runtime_error("too few horizontal sync pulses found");

    // Smoothed start of every lattice line: local quadratic fit over the
    // measurements within +-12 lines, after rejecting outliers against a first
    // fit.  Lines with no measurement of their own are interpolated the same way.
    const int64_t k_max = meas.back().k;
    vector<double> T((size_t)k_max + 2, c_nan);
    vector<uint8_t> supported((size_t)k_max + 2, 0);
    {
        constexpr int c_half = 12;
        size_t lo = 0, hi = 0;
        vector<pair<double, double>> pts;
        for (int64_t k = 0; k <= k_max; k++) {
            while (lo < meas.size() && meas[lo].k < k - c_half) lo++;
            while (hi < meas.size() && meas[hi].k <= k + c_half) hi++;
            pts.clear();
            for (size_t i = lo; i < hi; i++) pts.emplace_back((double)meas[i].k, meas[i].t);
            if (pts.empty()) continue;
            double v = localFit(pts, (double)k);
            // Reject points more than 1 us from the first fit and refit
            const double gate = 1.0 * opt.sample_rate / 1e6;
            vector<pair<double, double>> kept;
            for (auto &[x, y] : pts) {
                // Predicted at x from the fit at k with the running slope
                const double pred = v + (x - k) * p_run;
                if (fabs(y - pred) < gate) kept.push_back({x, y});
            }
            if (kept.size() != pts.size() && !kept.empty()) v = localFit(kept, (double)k);
            T[k] = v;
            for (auto &[x, y] : kept)
                if ((int64_t)x == k && fabs(y - v) < gate) supported[k] = 1;
        }
        // The line after the last: extrapolate
        T[k_max + 1] = T[k_max] + p_run;
    }

    // --- frames: each field-1 vertical group starts a frame at the nearest
    // lattice line; the field-2 group that follows adds its VBI codes ---
    vector<FrameRecord> frames;
    auto nearestLine = [&](double t) -> int64_t {
        // Binary search in T (monotonic where defined)
        int64_t lo = 0, hi = k_max;
        while (hi - lo > 1) {
            const int64_t mid = (lo + hi) / 2;
            (isnan(T[mid]) || T[mid] <= t ? lo : hi) = mid;
        }
        return fabs(T[lo] - t) < fabs(T[hi] - t) ? lo : hi;
    };
    for (size_t i = 0; i < fields.size(); i++) {
        if (!fields[i].field1) continue;
        FrameRecord fr;
        fr.seq = (int)frames.size();
        fr.k_line1 = nearestLine(fields[i].broad_start);
        if (fabs(T[fr.k_line1] - fields[i].broad_start) > 4 * opt.sample_rate / 1e6)
            continue; // the group is not on the lattice: unusable
        fr.starts.resize(opt.lines_per_frame + 1, c_nan);
        for (int l = 0; l <= opt.lines_per_frame; l++) {
            const int64_t k = fr.k_line1 + l;
            if (k <= k_max + 1) fr.starts[l] = T[k];
            if (l < opt.lines_per_frame && k <= k_max && supported[k]) fr.supported_lines++;
        }
        fr.start = fr.starts[0];
        int n = 0;
        double sum = 0;
        for (int l = 0; l < opt.lines_per_frame; l++)
            if (!isnan(fr.starts[l]) && !isnan(fr.starts[l + 1])) { sum += fr.starts[l + 1] - fr.starts[l]; n++; }
        fr.samples_per_line = n > 0 ? sum / n : c_nan;
        vector<int> codes = fields[i].codes;
        if (i + 1 < fields.size() && !fields[i + 1].field1
            && fields[i + 1].broad_start - fields[i].broad_start < 0.6 * opt.lines_per_frame * line_samples_in)
            codes.insert(codes.end(), fields[i + 1].codes.begin(), fields[i + 1].codes.end());
        fr.vbi = interpretVbi(codes);
        frames.push_back(std::move(fr));
    }
    // Picture keys, with the gaps filled by continuity: a frame whose picture
    // number did not decode (a dropout on the VBI lines) is the successor of
    // the previous frame's picture when the next decoded frame agrees, and
    // otherwise gets no key.  A CLV picture-in-second missing while the time
    // code decoded would give the ambiguous key "second * 100"; that too is
    // treated as missing.
    const int c_pictures_per_second = opt.lines_per_frame == 625 ? 25 : 30;
    auto successor = [&](int64_t key, const VbiInfo &like) -> int64_t {
        if (!like.is_clv) return key + 1;
        const int64_t sec = key / 100, pic = key % 100;
        return pic >= c_pictures_per_second ? (sec + 1) * 100 + 1 : sec * 100 + pic + 1;
    };
    for (auto &fr : frames) {
        fr.picture_key = fr.vbi.pictureKey();
        if (fr.vbi.is_clv && fr.vbi.clv_picture < 0) fr.picture_key = -1;
    }
    int inferred = 0;
    for (size_t i = 0; i < frames.size(); i++) {
        if (frames[i].picture_key >= 0) continue;
        // Walk back to the last keyed frame and forward to the next
        size_t a = i; while (a > 0 && frames[a - 1].picture_key < 0) a--;
        size_t b = i; while (b + 1 < frames.size() && frames[b + 1].picture_key < 0) b++;
        if (a == 0 || b + 1 >= frames.size()) continue;
        const VbiInfo &like = frames[a - 1].vbi;
        int64_t expect = frames[a - 1].picture_key;
        for (size_t j = a; j <= b + 1; j++) expect = successor(expect, like);
        if (expect != frames[b + 1].picture_key) continue; // frames missing or a false code: leave unkeyed
        int64_t key = frames[a - 1].picture_key;
        for (size_t j = a; j <= b; j++) { key = successor(key, like); frames[j].picture_key = key; frames[j].inferred_key = true; inferred++; }
    }
    if (opt.verbose)
        fprintf(stderr, "  %zu frames, %zu lattice lines, %.4f input samples per line, %d picture numbers inferred\n",
                frames.size(), (size_t)k_max + 1, p_run, inferred);
    return frames;
}

void Timebase::save(const string &prefix, const vector<FrameRecord> &frames, int lines_per_frame) {
    FILE *t = fopen((prefix + ".frames.tsv").c_str(), "w");
    FILE *b = fopen((prefix + ".lines.f64").c_str(), "wb");
    if (t == nullptr || b == nullptr) throw runtime_error("cannot write " + prefix + ".frames.tsv/.lines.f64");
    fprintf(t, "# lines_per_frame %d\n# seq\tk_line1\tstart\tsamples_per_line\tsupported_lines\tpicture_key\tvbi\tcodes\n", lines_per_frame);
    for (const auto &fr : frames) {
        string codes;
        for (int c : fr.vbi.codes) codes += format("{:06x} ", c & 0xffffff);
        fprintf(t, "%d\t%lld\t%.3f\t%.6f\t%d\t%lld\t%s%s\t%s\n", fr.seq, (long long)fr.k_line1, fr.start, fr.samples_per_line,
                fr.supported_lines, (long long)fr.picture_key, fr.vbi.describe().c_str(),
                fr.inferred_key ? " (inferred)" : "", codes.c_str());
        fwrite(fr.starts.data(), sizeof(double), fr.starts.size(), b);
    }
    fclose(t);
    fclose(b);
}

Timebase Timebase::load(const string &prefix) {
    Timebase tb;
    FILE *t = fopen((prefix + ".frames.tsv").c_str(), "r");
    FILE *b = fopen((prefix + ".lines.f64").c_str(), "rb");
    if (t == nullptr || b == nullptr) throw runtime_error("cannot open " + prefix + ".frames.tsv/.lines.f64");
    char line[4096];
    if (fgets(line, sizeof line, t) == nullptr || sscanf(line, "# lines_per_frame %d", &tb.lines_per_frame) != 1)
        throw runtime_error("bad header in " + prefix + ".frames.tsv");
    while (fgets(line, sizeof line, t) != nullptr) {
        if (line[0] == '#') continue;
        FrameRecord fr;
        long long k1, key;
        char vbi_text[256];
        if (sscanf(line, "%d\t%lld\t%lf\t%lf\t%d\t%lld\t%255[^\t\n]", &fr.seq, &k1, &fr.start, &fr.samples_per_line,
                   &fr.supported_lines, &key, vbi_text) < 6)
            continue;
        fr.k_line1 = k1;
        fr.picture_key = key;
        fr.inferred_key = strstr(vbi_text, "(inferred)") != nullptr;
        fr.starts.resize(tb.lines_per_frame + 1);
        if (fread(fr.starts.data(), sizeof(double), fr.starts.size(), b) != fr.starts.size())
            throw runtime_error("short read in " + prefix + ".lines.f64");
        tb.frames.push_back(std::move(fr));
    }
    fclose(t);
    fclose(b);
    return tb;
}

const FrameRecord *Timebase::findPicture(int64_t key) const {
    if (key < 0) return nullptr;
    for (const auto &fr : frames)
        if (fr.picture_key == key) return &fr;
    return nullptr;
}
