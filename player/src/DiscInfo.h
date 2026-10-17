// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_DISCINFO_H
#define MUSECPP_DISCINFO_H

#include <vector>
#include <string>
#include <optional>

class DiscInfo {
public:
    [[nodiscard]] virtual std::vector<std::string> asStrings() const = 0;

    // Best-effort wall-clock playback time on the disc, in seconds from start.
    // Returns nullopt when this field's disc metadata does not carry a usable
    // time (e.g. CAV with no frame number, or a corrupt code). Used by the
    // subtitle overlay to sync SRT entries to disc position.
    [[nodiscard]] virtual std::optional<double> playbackTimeSeconds() const {
        return std::nullopt;
    }

    // The format-independent navigation facts, for the chapter search and
    // the picture stop.  Each is unset or false when the format or this
    // particular frame does not carry it.
    [[nodiscard]] virtual std::optional<int> chapter() const { return std::nullopt; }
    [[nodiscard]] virtual bool isLeadIn() const { return false; }
    [[nodiscard]] virtual bool isLeadOut() const { return false; }
    // The disc asks the player to stop on this picture (a CAV still frame)
    [[nodiscard]] virtual bool pictureStop() const { return false; }

    virtual ~DiscInfo() = default;

protected:
    DiscInfo() = default;
};

#endif //MUSECPP_DISCINFO_H
