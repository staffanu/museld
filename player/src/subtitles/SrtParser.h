// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#ifndef MUSELD_SRT_PARSER_H
#define MUSELD_SRT_PARSER_H

#include <filesystem>
#include <string>
#include <vector>

struct SubtitleEntry {
    double start_seconds = 0.0;
    double end_seconds = 0.0;
    // Each line is UTF-8, with HTML-like tags already stripped and basic
    // entities (&amp; &lt; &gt; &quot; &#NNN;) decoded.
    std::vector<std::string> lines;
};

// Parse an SRT file into a vector sorted by start_seconds. Throws std::runtime_error
// on I/O errors. Malformed cues are skipped silently.
std::vector<SubtitleEntry> parseSrt(const std::filesystem::path &file);

// Parse from an in-memory string (useful for tests).
std::vector<SubtitleEntry> parseSrtText(const std::string &text);

#endif // MUSELD_SRT_PARSER_H
