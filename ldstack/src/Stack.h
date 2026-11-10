// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef LDSTACK_STACK_H
#define LDSTACK_STACK_H

#include <string>
#include <vector>

struct StackOptions {
    int max_lag = 4;          // integer lags searched per line, +- samples
    double min_cc = 0.5;      // a capture's line below this correlation with the reference is left out
    int window = 64;          // dropout weighting window, samples
    double dropout_ratio = 0.6; // window RMS below this fraction of the best capture's counts as a dropout
    bool verbose = false;
};

// Stack line-locked RF files (the <out> prefixes of `extract`) of the same
// pressing: the first is the reference grid, the others are aligned to it per
// line (sub-sample lag from broadband cross-correlation, gain match), and the
// captures are averaged with dropout windows left out.  Writes <out>.rf.i16,
// <out>.idx.tsv and <out>.stack.f64 (per line: seq, line, then per input
// lag, cc, gain, dropout fraction).
void stackCaptures(const std::string &out, const std::vector<std::string> &inputs, const StackOptions &options);

// The same for captures of different masterings of one programme, whose RF
// carriers are unrelated but whose video is the same: every line is FM
// demodulated (carrier_hz, output at half the RF rate), aligned to the
// reference per line, and averaged with dropout windows (from the RF
// envelope) left out.  Writes <out>.bb.f32 (instantaneous frequency in Hz,
// samples_per_line / 2 per line), <out>.idx.tsv and <out>.stack.f64 (per
// line: seq, line, then per input lag, cc, dropout fraction).  With one input
// this is plain demodulation.
void stackBaseband(const std::string &out, const std::vector<std::string> &inputs, double carrier_hz,
                   const StackOptions &options);

#endif //LDSTACK_STACK_H
