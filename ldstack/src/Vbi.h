// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef LDSTACK_VBI_H
#define LDSTACK_VBI_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The Philips codes (IEC 60857 §10.1) on the vertical blanking lines: 24-bit
// biphase words that carry the picture number, time code, chapter and disc
// status.  This is a port of museld's NtscFrame VBI decoding to the offline
// demodulated signal.

// One code word from a line's demodulated instantaneous frequency (Hz), given
// the line's start sample, samples per line, and the blanking frequency.  -1
// when the line carries no valid code.
int decodePhilipsLine(const float *line_start, double samples_per_line, double blanking_hz, double white_hz);

struct VbiInfo {
    bool valid = false;      // any code decoded
    bool is_clv = false;
    bool lead_in = false, lead_out = false, stop_code = false;
    int chapter = -1;
    int cav_picture = -1;
    int clv_time_seconds = -1;
    int clv_picture = -1;
    std::optional<bool> cx;
    std::vector<int> codes;  // the raw words, for diagnostics

    // Sortable identity of the disc picture, comparable between captures of
    // the same disc: the CAV picture number, or for CLV time * 100 + picture
    [[nodiscard]] int64_t pictureKey() const;
    [[nodiscard]] std::string describe() const;
};

// Interpret the code words of one frame (both fields' VBI lines)
VbiInfo interpretVbi(const std::vector<int> &codes);

#endif //LDSTACK_VBI_H
