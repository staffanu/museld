// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)
//
// ldstack: line-locked extraction, alignment and stacking of laserdisc RF
// captures of the same pressing.  See README.md.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>
#include "LdsFile.h"
#include "Scanner.h"
#include "Resample.h"
#include "Stack.h"

int cmdEfmPredict(int argc, char **argv);
int cmdEfmBits(int argc, char **argv);
int cmdEfmExport(int argc, char **argv);
int cmdEfmScore(int argc, char **argv);
int cmdEfmC1(int argc, char **argv);

using namespace std;

namespace {
    void usage() {
        cerr << "usage: ldstack scan <capture.lds> <prefix> [--pal|--ntsc] [--seconds S] [-v]\n"
                "         Find every video line and picture number; writes <prefix>.frames.tsv and .lines.f64\n"
                "       ldstack extract <capture.lds> <prefix> <out> [--pictures A-B] [--samples-per-line N] [--stats-only]\n"
                "         Resample the RF of each line to N samples (default 2560) as int16 x16 into <out>.rf.i16\n"
                "         with an index in <out>.idx.tsv, and per-line period and RMS into <out>.stats.f64\n"
                "       ldstack stack <out> <rf1> <rf2> [<rf3> ...] [--max-lag N] [--min-cc C] [--window W] [-v]\n"
                "         Align the extracted RF files (the <out> prefixes of extract) of the same pressing to the\n"
                "         first one per line and average them, leaving dropout windows out; same output layout\n"
                "       ldstack stackbb <out> <rf1> [<rf2> ...] [--carrier HZ] [--max-lag N] [--min-cc C] [--window W] [-v]\n"
                "         FM demodulate the extracted RF files (different masterings of one programme are fine),\n"
                "         align per line and average into <out>.bb.f32 (Hz, half the RF rate); one input just demodulates\n"
                "       ldstack efmpredict <audio.pcm> <circ-dump.txt> [--range MIN-MAX] [--report FILE]\n"
                "         CIRC-encode the known audio (s16le stereo) and compare the predicted EFM data symbols with\n"
                "         the frames an ac3rf-efm-decode --circ-debug-filename dump received; finds the frame phase and offset\n";
        exit(EXIT_FAILURE);
    }

    int cmdScan(int argc, char **argv) {
        if (argc < 4) usage();
        ScanOptions opt;
        for (int i = 4; i < argc; i++) {
            const string a = argv[i];
            if (a == "--ntsc") { opt.carrier_hz = 8.1e6; opt.white_hz_offset = 1.2e6; opt.lines_per_frame = 525; opt.line_us = 1e6 / 15734.264; }
            else if (a == "--pal") { opt.carrier_hz = 7.1e6; opt.white_hz_offset = 0.8e6; opt.lines_per_frame = 625; opt.line_us = 64.0; }
            else if (a == "--seconds" && i + 1 < argc) opt.max_samples = (int64_t)(atof(argv[++i]) * opt.sample_rate);
            else if (a == "-v") opt.verbose = true;
            else usage();
        }
        auto frames = scanCapture(argv[2], opt);
        Timebase::save(argv[3], frames, opt.lines_per_frame);
        int with_vbi = 0;
        for (auto &f : frames) if (f.picture_key >= 0) with_vbi++;
        cout << format("{} frames, {} with a picture number", frames.size(), with_vbi);
        if (!frames.empty()) {
            const FrameRecord *first = nullptr, *last = nullptr;
            for (auto &f : frames) if (f.picture_key >= 0) { if (!first) first = &f; last = &f; }
            if (first) cout << format(", {} .. {}", first->vbi.describe(), last->vbi.describe());
        }
        cout << endl;
        return 0;
    }

    int cmdExtract(int argc, char **argv) {
        if (argc < 5) usage();
        int64_t pic_a = -1, pic_b = -1;
        int n_out = 2560;
        bool stats_only = false;
        for (int i = 5; i < argc; i++) {
            const string a = argv[i];
            if (a == "--pictures" && i + 1 < argc) { if (sscanf(argv[++i], "%lld-%lld", (long long *)&pic_a, (long long *)&pic_b) != 2) usage(); }
            else if (a == "--samples-per-line" && i + 1 < argc) n_out = atoi(argv[++i]);
            else if (a == "--stats-only") stats_only = true;
            else usage();
        }
        LdsFile file(argv[2]);
        Timebase tb = Timebase::load(argv[3]);
        const string out = argv[4];
        FILE *rf = stats_only ? nullptr : fopen((out + ".rf.i16").c_str(), "wb");
        FILE *idx = fopen((out + ".idx.tsv").c_str(), "w");
        FILE *st = fopen((out + ".stats.f64").c_str(), "wb");
        if ((rf == nullptr && !stats_only) || idx == nullptr || st == nullptr) throw runtime_error("cannot write " + out + ".*");
        fprintf(idx, "# lines_per_frame %d samples_per_line %d\n# seq\tpicture_key\tk_line1\tstart\tsamples_per_line\tbyte_offset\n",
                tb.lines_per_frame, n_out);
        Resampler rs;
        vector<int16_t> line_out(n_out);
        int64_t byte_offset = 0;
        int written = 0;
        for (const auto &fr : tb.frames) {
            const int64_t key = fr.picture_key;
            if (pic_a >= 0 && (key < pic_a || key > pic_b)) continue;
            fprintf(idx, "%d\t%lld\t%lld\t%.3f\t%.6f\t%lld\n", fr.seq, (long long)key, (long long)fr.k_line1, fr.start,
                    fr.samples_per_line, (long long)byte_offset);
            for (int l = 0; l < tb.lines_per_frame; l++) {
                const double t0 = fr.starts[l], t1 = fr.starts[l + 1];
                double stats[4] = {(double)fr.seq, (double)l, t1 - t0, 0.0};
                if (isnan(t0) || isnan(t1) || t1 <= t0) {
                    fill(line_out.begin(), line_out.end(), 0);
                    stats[2] = 0;
                } else {
                    const int64_t first = (int64_t)floor(t0) - 8;
                    const int64_t count = (int64_t)ceil(t1) - first + 16;
                    const float *in = file.samples(first, count);
                    double sumsq = 0;
                    const double step = (t1 - t0) / n_out;
                    for (int j = 0; j < n_out; j++) {
                        const double pos = t0 + j * step;
                        const int64_t ip = (int64_t)floor(pos);
                        const float v = rs.at(in + (ip - first), pos - ip);
                        sumsq += (double)v * v;
                        const float scaled = v * 16.0f;
                        line_out[j] = (int16_t)max(-32768.0f, min(32767.0f, roundf(scaled)));
                    }
                    stats[3] = sqrt(sumsq / n_out);
                }
                if (rf) fwrite(line_out.data(), sizeof(int16_t), line_out.size(), rf);
                fwrite(stats, sizeof stats, 1, st);
            }
            byte_offset += (int64_t)tb.lines_per_frame * n_out * sizeof(int16_t);
            written++;
        }
        if (rf) fclose(rf);
        fclose(idx);
        fclose(st);
        cout << format("{} frames written", written) << endl;
        return 0;
    }
}

namespace {
    int cmdStack(int argc, char **argv) {
        if (argc < 5) usage();
        StackOptions opt;
        vector<string> inputs;
        for (int i = 3; i < argc; i++) {
            const string a = argv[i];
            if (a == "--max-lag" && i + 1 < argc) opt.max_lag = atoi(argv[++i]);
            else if (a == "--min-cc" && i + 1 < argc) opt.min_cc = atof(argv[++i]);
            else if (a == "--window" && i + 1 < argc) opt.window = atoi(argv[++i]);
            else if (a == "-v") opt.verbose = true;
            else if (a[0] == '-') usage();
            else inputs.push_back(a);
        }
        stackCaptures(argv[2], inputs, opt);
        return 0;
    }

    int cmdStackBaseband(int argc, char **argv) {
        if (argc < 4) usage();
        StackOptions opt;
        double carrier = 7.1e6;
        vector<string> inputs;
        for (int i = 3; i < argc; i++) {
            const string a = argv[i];
            if (a == "--max-lag" && i + 1 < argc) opt.max_lag = atoi(argv[++i]);
            else if (a == "--min-cc" && i + 1 < argc) opt.min_cc = atof(argv[++i]);
            else if (a == "--window" && i + 1 < argc) opt.window = atoi(argv[++i]);
            else if (a == "--carrier" && i + 1 < argc) carrier = atof(argv[++i]);
            else if (a == "-v") opt.verbose = true;
            else if (a[0] == '-') usage();
            else inputs.push_back(a);
        }
        stackBaseband(argv[2], inputs, carrier, opt);
        return 0;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) usage();
    try {
        const string cmd = argv[1];
        if (cmd == "scan") return cmdScan(argc, argv);
        if (cmd == "extract") return cmdExtract(argc, argv);
        if (cmd == "stack") return cmdStack(argc, argv);
        if (cmd == "stackbb") return cmdStackBaseband(argc, argv);
        if (cmd == "efmpredict") return cmdEfmPredict(argc, argv);
        if (cmd == "efmbits") return cmdEfmBits(argc, argv);
        if (cmd == "efmexport") return cmdEfmExport(argc, argv);
        if (cmd == "efmscore") return cmdEfmScore(argc, argv);
        if (cmd == "efmc1") return cmdEfmC1(argc, argv);
        usage();
    } catch (const exception &e) {
        cerr << "ldstack: " << e.what() << endl;
        return EXIT_FAILURE;
    }
    return 0;
}
