// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_SAMPLECAPTURE_H
#define MUSELD_SAMPLECAPTURE_H

#include <filesystem>
#include <optional>
#include <string>
#include "ui/SavedState.h"

// The sample capture shipped with the player (the tvOS bundle; a desktop build
// finds it the same way when it is put there): ten seconds of a Space Shuttle
// launch from the SpaceDisc vol. 1 laserdisc (Video Vision Associates,
// SS098-0001), the mission report footage NASA produced, which is a US
// government work and so free of copyright, cut from frame 2842 of side 1
// (CAV, 33.33 MHz 8-bit fx3usbadc capture).  The disc's analog channels carry
// a language each: the right one NASA's English narration, the left one the
// publisher's Japanese, so the sample is played with the right channel alone
// unless the viewer chooses otherwise.
namespace sample_capture {
    constexpr const char *c_relative_path = "samples/spacedisc-sts-launch-10s.ldf";
    constexpr const char *c_label = "Play the sample capture (a Space Shuttle launch)";

    // The sample beside the executable, if it is there
    inline std::optional<std::string> locate(const std::string &executable_dir) {
        const auto path = std::filesystem::path(executable_dir) / c_relative_path;
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec))
            return path.string();
        return std::nullopt;
    }

    inline bool isSample(const std::string &input_name) {
        const std::string name = std::filesystem::path(c_relative_path).filename().string();
        return input_name.size() >= name.size() && input_name.compare(input_name.size() - name.size(), name.size(), name) == 0;
    }

    // What the sample is played with before any memory of it: the English channel
    inline DiscSettings settings() {
        DiscSettings d;
        d.channels = "right";
        return d;
    }
}

#endif //MUSELD_SAMPLECAPTURE_H
