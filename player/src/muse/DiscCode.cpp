// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#include "DiscCode.h"
#include <format>

DiscCode::DiscCode(int mode, int cadr, int fadr1, int fadr2) :
        DiscInfo(),
        m_mode(mode),
        m_cadr(cadr),
        m_fadr1(fadr1),
        m_fadr2(fadr2) {
}

std::vector<std::string> DiscCode::asStrings() const {
    std::string disc_code_string1 =
        std::format("{}{} {}", pf() ? "TOC " : "", sz() ? "20 cm" : "30 cm", df() ? "CLV" : "CAV");
    std::string disc_code_string2 = std::format("Chapter {} Frame {}", chapter(), frame());

    return {disc_code_string1, disc_code_string2};
}

std::optional<double> DiscCode::playbackTimeSeconds() const {
    // MUSE is 60 fields/30 progressive frames per second. The disc-code frame
    // counter starts at 1.
    if (pf()) return std::nullopt; // lead-in / TOC has no playback time
    const int f = frame();
    if (f <= 0) return std::nullopt;
    return (f - 1) / 30.0;
}
