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

    // The format-independent navigation facts, for the position search and
    // the picture stop.  Each is unset or false when the format or this
    // particular frame does not carry it.
    [[nodiscard]] virtual std::optional<int> chapter() const { return std::nullopt; }
    // The disc's time code in its own seconds, as a player displays it: a
    // second of it is a whole number of frames (30 on NTSC, which play in
    // 1.001 s), so it drifts from playbackTimeSeconds().  CLV discs carry
    // it; on a CAV disc it is counted from the frame number.
    [[nodiscard]] virtual std::optional<double> discTimeSeconds() const { return std::nullopt; }
    // The step of that time: a frame where the code numbers the frames
    // within the second, a whole second where it does not
    [[nodiscard]] virtual double discTimeResolutionSeconds() const { return 1.0; }
    // The picture number of a CAV disc (counted from 1 at the programme
    // start); unset on CLV discs and on frames without the code
    [[nodiscard]] virtual std::optional<int> frameNumber() const { return std::nullopt; }
    // Whether the disc is CLV (time code) or CAV (frame numbers): what a
    // position search can be asked for.  Unset while the code has not said.
    [[nodiscard]] virtual std::optional<bool> isClv() const { return std::nullopt; }
    [[nodiscard]] virtual bool isLeadIn() const { return false; }
    [[nodiscard]] virtual bool isLeadOut() const { return false; }
    // The disc asks the player to stop on this picture (a CAV still frame)
    [[nodiscard]] virtual bool pictureStop() const { return false; }

    virtual ~DiscInfo() = default;

protected:
    DiscInfo() = default;
};

#endif //MUSECPP_DISCINFO_H
