// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#include "SrtParser.h"

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

void stripTrailingCr(std::string &s) {
    if (!s.empty() && s.back() == '\r') s.pop_back();
}

bool isBlank(const std::string &s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
}

// Parse "HH:MM:SS,mmm" or "HH:MM:SS.mmm" into seconds. Returns -1 on failure.
double parseTimestamp(const std::string &s) {
    int h, m, sec, ms;
    char sep;
    std::istringstream is(s);
    is >> h;
    if (is.get() != ':') return -1;
    is >> m;
    if (is.get() != ':') return -1;
    is >> sec;
    sep = is.get();
    if (sep != ',' && sep != '.') return -1;
    is >> ms;
    if (is.fail()) return -1;
    return h * 3600.0 + m * 60.0 + sec + ms / 1000.0;
}

// Parse "HH:MM:SS,mmm --> HH:MM:SS,mmm" line. Returns true on success.
bool parseTimeLine(const std::string &line, double &start, double &end) {
    // Find the arrow.
    auto arrow = line.find("-->");
    if (arrow == std::string::npos) return false;
    std::string lhs = line.substr(0, arrow);
    std::string rhs = line.substr(arrow + 3);
    auto trim = [](std::string &s) {
        size_t a = s.find_first_not_of(" \t");
        size_t b = s.find_last_not_of(" \t");
        if (a == std::string::npos) { s.clear(); return; }
        s = s.substr(a, b - a + 1);
        // Cut off any positioning extension after the second timestamp
        size_t sp = s.find(' ');
        if (sp != std::string::npos) s = s.substr(0, sp);
    };
    trim(lhs);
    trim(rhs);
    start = parseTimestamp(lhs);
    end = parseTimestamp(rhs);
    return start >= 0 && end >= 0;
}

std::string decodeEntities(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        if (in[i] == '&') {
            auto semi = in.find(';', i + 1);
            if (semi != std::string::npos && semi - i <= 8) {
                std::string ent = in.substr(i + 1, semi - i - 1);
                if (ent == "amp") { out.push_back('&'); i = semi + 1; continue; }
                if (ent == "lt")  { out.push_back('<'); i = semi + 1; continue; }
                if (ent == "gt")  { out.push_back('>'); i = semi + 1; continue; }
                if (ent == "quot") { out.push_back('"'); i = semi + 1; continue; }
                if (ent == "apos") { out.push_back('\''); i = semi + 1; continue; }
                if (!ent.empty() && ent[0] == '#') {
                    int code = 0;
                    bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
                    try {
                        code = std::stoi(ent.substr(hex ? 2 : 1), nullptr, hex ? 16 : 10);
                    } catch (...) { code = 0; }
                    if (code > 0) {
                        // Encode as UTF-8.
                        if (code < 0x80) {
                            out.push_back(static_cast<char>(code));
                        } else if (code < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        } else if (code < 0x10000) {
                            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
                            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        }
                        i = semi + 1;
                        continue;
                    }
                }
            }
        }
        out.push_back(in[i++]);
    }
    return out;
}

std::string stripTags(const std::string &in) {
    static const std::regex tag_re("<[^>]*>");
    return std::regex_replace(in, tag_re, "");
}

} // namespace

std::vector<SubtitleEntry> parseSrtText(const std::string &text) {
    std::vector<SubtitleEntry> entries;

    // Skip a UTF-8 BOM if present.
    size_t pos = 0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF
        && static_cast<unsigned char>(text[1]) == 0xBB
        && static_cast<unsigned char>(text[2]) == 0xBF) {
        pos = 3;
    }

    std::vector<std::string> lines;
    while (pos <= text.size()) {
        auto nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        stripTrailingCr(line);
        lines.push_back(std::move(line));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }

    size_t i = 0;
    while (i < lines.size()) {
        // Skip blank lines.
        while (i < lines.size() && isBlank(lines[i])) ++i;
        if (i >= lines.size()) break;

        // The next line may be an index (digits only) or already the timestamp
        // line. Accept either.
        size_t time_idx = i;
        if (lines[i].find("-->") == std::string::npos && i + 1 < lines.size()
            && lines[i + 1].find("-->") != std::string::npos) {
            time_idx = i + 1;
        }

        if (time_idx >= lines.size()) break;

        SubtitleEntry e;
        if (!parseTimeLine(lines[time_idx], e.start_seconds, e.end_seconds)) {
            // Skip a single line and try to resync.
            i = time_idx + 1;
            continue;
        }

        // Collect text lines until blank.
        size_t j = time_idx + 1;
        while (j < lines.size() && !isBlank(lines[j])) {
            std::string cleaned = decodeEntities(stripTags(lines[j]));
            e.lines.push_back(std::move(cleaned));
            ++j;
        }

        if (!e.lines.empty()) entries.push_back(std::move(e));
        i = j;
    }

    std::sort(entries.begin(), entries.end(),
        [](const SubtitleEntry &a, const SubtitleEntry &b) {
            return a.start_seconds < b.start_seconds;
        });
    return entries;
}

std::vector<SubtitleEntry> parseSrt(const std::filesystem::path &file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Could not open subtitle file: " + file.string());
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return parseSrtText(buf.str());
}
