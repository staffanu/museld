// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <cstring>
#include "Format.h"

namespace {

// Indexed by Format; keep in the same order as the enum.
constexpr FormatInfo k_formats[] = {
    { "lds",   10,   512,      0,  1023, 4, 5 },
    { "r30",   10,   512,      0,  1023, 3, 4 },
    { "s8",     8,     0,   -128,   127, 1, 1 },
    { "u8",     8,   128,      0,   255, 1, 1 },
    { "s16",   16,     0, -32768, 32767, 1, 2 },
    { "u16",   16, 32768,      0, 65535, 1, 2 },
    { "s16be", 16,     0, -32768, 32767, 1, 2 },
    { "u16be", 16, 32768,      0, 65535, 1, 2 },
    // Floats are converted to and from a 16-bit code domain on the way in and
    // out, matching ld-decode, which reads .rf as float * 32768.
    { "f32",   16,     0, -32768, 32767, 1, 4 },
    { "flac",  16,     0, -32768, 32767, 1, 0 },
    { "ldf",   16,     0, -32768, 32767, 1, 0 },
};

std::string toLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

} // namespace

const FormatInfo &formatInfo(Format format) {
    return k_formats[(size_t)format];
}

std::optional<Format> formatFromName(std::string_view name) {
    const std::string n = toLower(name);
    for (size_t i = 0; i < std::size(k_formats); i++)
        if (n == k_formats[i].name)
            return (Format)i;

    // Names ld-decode and the capture tools use for the same things.
    if (n == "r8" || n == "raw8")     return Format::U8;
    if (n == "r16" || n == "raw16")   return Format::U16;
    if (n == "raw")                   return Format::S16;
    if (n == "rf")                    return Format::F32;
    if (n == "oga" || n == "raw.oga") return Format::OggFlac;
    if (n == "flac.ldf")              return Format::Flac;
    return std::nullopt;
}

std::optional<Format> formatFromFilename(std::string_view path) {
    const std::string p = toLower(path);
    const auto ends = [&p](std::string_view suffix) { return p.ends_with(suffix); };

    // The compound extensions have to be tested before their tails.
    if (ends(".flac.ldf")) return Format::Flac;
    if (ends(".raw.oga"))  return Format::OggFlac;
    if (ends(".ldf"))      return Format::OggFlac;
    if (ends(".oga"))      return Format::OggFlac;
    if (ends(".flac"))     return Format::Flac;
    if (ends(".lds"))      return Format::Lds;
    if (ends(".r30"))      return Format::R30;
    if (ends(".s16be"))    return Format::S16BE;
    if (ends(".u16be"))    return Format::U16BE;
    if (ends(".s16"))      return Format::S16;
    if (ends(".raw"))      return Format::S16;
    if (ends(".u16"))      return Format::U16;
    if (ends(".r16"))      return Format::U16;
    if (ends(".s8"))       return Format::S8;
    if (ends(".u8"))       return Format::U8;
    if (ends(".r8"))       return Format::U8;
    if (ends(".rf"))       return Format::F32;
    return std::nullopt;
}

std::optional<Format> formatFromMagic(const unsigned char *data, size_t size) {
    if (size >= 4 && memcmp(data, "fLaC", 4) == 0) return Format::Flac;
    if (size >= 4 && memcmp(data, "OggS", 4) == 0) return Format::OggFlac;
    return std::nullopt;
}

std::string formatNameList() {
    std::string out;
    for (const auto &info : k_formats) {
        if (!out.empty())
            out += ", ";
        out += info.name;
    }
    return out;
}
