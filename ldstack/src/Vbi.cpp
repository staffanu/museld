// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "Vbi.h"

#include <algorithm>
#include <format>

int decodePhilipsLine(const float *line_start, double samples_per_line, double blanking_hz, double white_hz) {
    // Normalized level: 0 at blanking, 1 at white
    auto level = [&](const float *p) { return (*p - blanking_hz) / (white_hz - blanking_hz); };
    const float *vbi = line_start + (int)(0.15 * samples_per_line);
    if (level(vbi) > 0.5)
        return -1; // signal present before the code can start
    // 64 us line: the sample rate follows from samples_per_line
    const double samples_per_us = samples_per_line / 64.0;
    bool found = false;
    for (int i = 0; i < (int)(5 * samples_per_us); i++, vbi++)
        if (level(vbi) > 0.7) {
            found = true;
            break;
        }
    if (!found)
        return -1;

    const int samples_per_half_bit = (int)(1.0 * samples_per_us);
    uint64_t half_bits = 0b01;
    int prev = 1;
    for (int i = 2; i < 48; i++) {
        int t = 0;
        while (t < samples_per_half_bit * 9 / 4) {
            const float v = level(vbi++);
            if ((prev && v < 0.5) || (!prev && v > 0.5)) {
                prev = 1 - prev;
                break;
            }
            t++;
        }
        if (t < samples_per_half_bit * 3 / 4)
            return -1;
        else if (t < samples_per_half_bit * 5 / 4)
            half_bits = (half_bits << 1) | (uint64_t)prev;
        else if (t < samples_per_half_bit * 7 / 4)
            return -1;
        else if (t < samples_per_half_bit * 9 / 4) {
            half_bits = (half_bits << 2) | ((uint64_t)(1 - prev) << 1) | (uint64_t)prev;
            i++;
        } else
            return -1;
    }
    int codeword = 0;
    for (int i = 0; i < 24; i++) {
        const int pair = (int)((half_bits >> (46 - 2 * i)) & 0b11);
        codeword <<= 1;
        if (pair == 0b01)
            codeword |= 1;
        else if (pair != 0b10)
            return -1;
    }
    return codeword;
}

VbiInfo interpretVbi(const std::vector<int> &codes) {
    VbiInfo v;
    v.codes = codes;
    auto count = [&](int code) { return (int)std::count(codes.begin(), codes.end(), code); };
    auto find = [&](int mask, int pattern) {
        auto it = std::find_if(codes.begin(), codes.end(), [&](int c) { return (c & mask) == pattern; });
        return it == codes.end() ? -1 : *it;
    };
    auto isBcdPictureNumber = [](int code) {
        if ((code & 0xf00000) != 0xf00000) return false;
        for (int shift = 0; shift < 20; shift += 4)
            if (((code >> shift) & 0xf) > 9) return false;
        return true;
    };
    v.valid = !codes.empty();
    v.lead_in = count(0x88FFFF) >= 2;
    v.lead_out = count(0x80EEEE) >= 2;
    v.is_clv = count(0x87ffff) >= 1;
    v.stop_code = count(0x82CFFF) >= 2;
    const int clv_time_data = v.is_clv ? find(0xf0ff00, 0xf0dd00) : -1;
    const int chapter_data = find(0xf00fff, 0x800ddd);
    // Chapter number "8 X1 X2 D D D": the top bit of X1 is a flag (set over
    // the first 400 frames of the chapter), not part of the number
    if (chapter_data != -1)
        v.chapter = (((chapter_data & 0xf0000) >> 16) & 7) * 10 + ((chapter_data & 0xf000) >> 12);
    if (!v.is_clv) {
        auto it = std::find_if(codes.begin(), codes.end(), [&](int c) { return isBcdPictureNumber(c) && count(c) >= 2; });
        if (it != codes.end())
            v.cav_picture = ((*it & 0xf0000) >> 16) * 10000 + ((*it & 0xf000) >> 12) * 1000 +
                            ((*it & 0xf00) >> 8) * 100 + ((*it & 0xf0) >> 4) * 10 + (*it & 0xf);
    } else {
        int t = clv_time_data != -1 ? ((clv_time_data & 0xf0000) >> 16) * 3600 + ((clv_time_data & 0xf0) >> 4) * 600 +
                                      (clv_time_data & 0xf) * 60 : -1;
        const int pic = find(0xf0f000, 0x80e000);
        if (pic != -1) {
            const int second = (((pic & 0xf0000) >> 16) - 10) * 10 + ((pic & 0xf00) >> 8);
            v.clv_picture = ((pic & 0xf0) >> 4) * 10 + (pic & 0xf);
            if (t != -1)
                t += second;
        }
        v.clv_time_seconds = t;
    }
    if (const int cx_on = find(0xfff000, 0x8dc000); cx_on != -1)
        v.cx = true;
    else if (const int cx_off = find(0xfff000, 0x8ba000); cx_off != -1)
        v.cx = false;
    return v;
}

int64_t VbiInfo::pictureKey() const {
    if (!valid) return -1;
    if (!is_clv) return cav_picture;
    if (clv_time_seconds < 0) return -1;
    return (int64_t)clv_time_seconds * 100 + (clv_picture < 0 ? 0 : clv_picture);
}

std::string VbiInfo::describe() const {
    if (!valid) return "no VBI";
    std::string s = is_clv ? "CLV" : "CAV";
    if (lead_in) s += " lead-in";
    if (lead_out) s += " lead-out";
    if (stop_code) s += " stop";
    if (chapter >= 0) s += std::format(" ch{}", chapter);
    if (is_clv) {
        if (clv_time_seconds >= 0)
            s += std::format(" {}:{:02}:{:02}", clv_time_seconds / 3600, clv_time_seconds % 3600 / 60, clv_time_seconds % 60);
        if (clv_picture >= 0) s += std::format(" pic {:02}", clv_picture);
    } else if (cav_picture >= 0)
        s += std::format(" frame {:05}", cav_picture);
    return s;
}
