// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef LDSTACK_SCANNER_H
#define LDSTACK_SCANNER_H

#include <cstdint>
#include <string>
#include <vector>
#include "Vbi.h"

// Analysis of one capture: where every video line starts, in input samples,
// and which disc picture each frame is.
//
// The scan demodulates the video FM carrier, slices the sync pulses off the
// instantaneous frequency, chains the horizontal sync pulses onto an integer
// line lattice and smooths their times with a local polynomial fit, so that
// disc wow (once per revolution) is followed while pulse timing noise and
// dropouts are averaged out.  Vertical sync groups anchor the frame line
// numbering, and the Philips codes on the VBI lines identify the pictures.
//
// The result is written as two files:
//   <prefix>.frames.tsv  one row per frame: seq, lattice line of line 1,
//                        start (input samples), samples per line, lines with a
//                        sync pulse of their own, picture key, VBI text
//   <prefix>.lines.f64   per frame, lines_per_frame + 1 doubles: the start of
//                        each line and the end of the last, in input samples
struct ScanOptions {
    double sample_rate = 40e6;
    double carrier_hz = 7.1e6;      // nominal blanking frequency (PAL 7.1, NTSC 8.1)
    double white_hz_offset = 0.8e6; // white above blanking (PAL 0.8, NTSC 1.2)
    int lines_per_frame = 625;
    double line_us = 64.0;
    int64_t max_samples = 0;        // 0 = whole file
    bool verbose = false;
};

struct FrameRecord {
    int seq = 0;
    int64_t k_line1 = 0;
    double start = 0;             // input sample of line 1
    double samples_per_line = 0;
    int supported_lines = 0;      // lines whose start came from their own sync pulse
    VbiInfo vbi;
    // Identity of the disc picture, comparable between captures of the same
    // disc: the VBI's picture key, or when a frame's code failed to decode,
    // the key inferred from its neighbours (inferred_key set); -1 if neither
    int64_t picture_key = -1;
    bool inferred_key = false;
    std::vector<double> starts;   // lines_per_frame + 1 entries
};

std::vector<FrameRecord> scanCapture(const std::string &lds_path, const ScanOptions &options);

// The same records read back from <prefix>.frames.tsv / .lines.f64
struct Timebase {
    int lines_per_frame = 0;
    std::vector<FrameRecord> frames;
    static Timebase load(const std::string &prefix);
    static void save(const std::string &prefix, const std::vector<FrameRecord> &frames, int lines_per_frame);
    const FrameRecord *findPicture(int64_t key) const;
};

#endif //LDSTACK_SCANNER_H
