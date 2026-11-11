// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_SUBTITLE_DISCOVERY_H
#define MUSELD_SUBTITLE_DISCOVERY_H

#include <string>
#include <utility>
#include <vector>
#include "SrtParser.h"

// The subtitle files that belong to an input: the .srt files beside it whose
// names start with the input's own name minus its extension -- capture.raw
// matches capture.srt, capture.ja.srt, capture.ja-hira.srt, ...  "Beside it"
// is the input's directory, or for an http:// input the web server's index
// page of that directory, so a capture played from a server (all an Apple TV
// has) finds its subtitles like one played from a disk.

struct SubtitleFile {
    std::string label;    // what follows the shared stem: "ja" for capture.ja.srt, "DEFAULT" for capture.srt
    std::string location; // a path or an http:// URL, for loadSubtitleFile

    bool operator==(const SubtitleFile &) const = default;
};

// Which of `names` (file names, not paths) are subtitle files of the input
// named `input_name`, as (label, name), sorted by name.  Pure, for the test.
std::vector<std::pair<std::string, std::string>> matchSubtitleNames(
        const std::string &input_name, const std::vector<std::string> &names);

// The subtitle files of an input (a path or an http:// URL), sorted by name;
// none for the other kinds of input, a directory that cannot be listed or a
// server without an index page.  Blocks for the listing of an http:// input.
std::vector<SubtitleFile> discoverSubtitleFiles(const std::string &input);

// Reads and parses a subtitle file from a path or an http:// URL.  Throws
// std::runtime_error when it cannot be read.
std::vector<SubtitleEntry> loadSubtitleFile(const std::string &location);

#endif // MUSELD_SUBTITLE_DISCOVERY_H
