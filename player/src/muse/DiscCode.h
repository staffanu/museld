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
    // MUSE runs at exactly 30 frames/s, so the code's time is the playback time
    std::optional<double> discTimeSeconds() const override { return playbackTimeSeconds(); }
    double discTimeResolutionSeconds() const override { return 1.0 / 30; }
    std::optional<int> chapter() const override { return pf() ? std::nullopt : std::make_optional(chapterNumber()); }
    // The frame address is on CLV discs too, but a player searches those by time
    std::optional<int> frameNumber() const override;
    std::optional<bool> isClv() const override { return df(); }
    bool isLeadIn() const override { return pf(); }
    // The MUSE disc code has a picture stop bit (ST in FADR2, EP 0 532 277),
    // but its bit position is only given in the patent's drawing; until a
    // disc with stops turns up it stays unread.

private:
    int m_mode;
    int m_cadr;
    int m_fadr1;
    int m_fadr2;

    [[nodiscard]] bool pf() const { return m_mode & 0x80; } // true if part of TOC (lead-in)
    [[nodiscard]] bool sz() const { return m_mode & 0x40; } // true if 20 cm disc
    [[nodiscard]] bool df() const { return m_mode & 0x20; } // true if CLV
    [[nodiscard]] int chapterNumber() const { return m_cadr & 0x7f; }
    [[nodiscard]] int frame() const { return m_fadr1 & 0x1ffff; }
};

#endif //MUSECPP_DISCCODE_H
