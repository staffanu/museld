//
// Created by staffanu on 6/22/24.
//

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

    virtual ~DiscInfo() = default;

protected:
    DiscInfo() = default;
};

#endif //MUSECPP_DISCINFO_H
