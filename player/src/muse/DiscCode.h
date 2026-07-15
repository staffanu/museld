// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_DISCCODE_H
#define MUSECPP_DISCCODE_H

#include <vector>
#include <string>
#include "DiscInfo.h"

class DiscCode : public DiscInfo {
public:
    DiscCode(int mode, int cadr, int fadr1, int fadr2);

    std::vector<std::string> asStrings() const override;
    std::optional<double> playbackTimeSeconds() const override;

private:
    int m_mode;
    int m_cadr;
    int m_fadr1;
    int m_fadr2;

    [[nodiscard]] bool pf() const { return m_mode & 0x80; } // true if part of TOC (lead-in)
    [[nodiscard]] bool sz() const { return m_mode & 0x40; } // true if 20 cm disc
    [[nodiscard]] bool df() const { return m_mode & 0x20; } // true if CLV
    [[nodiscard]] int chapter() const { return m_cadr & 0x7f; }
    [[nodiscard]] int frame() const { return m_fadr1 & 0x1ffff; }
};

#endif //MUSECPP_DISCCODE_H
