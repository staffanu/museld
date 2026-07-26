// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef LDCONV_FORMAT_H
#define LDCONV_FORMAT_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The sample formats used by laserdisc RF captures.  All of them are mono
// streams of integer code values; FormatInfo says how wide a code is and which
// code means "no signal", which is everything the conversion needs to know.
enum class Format {
    Lds,      // 10-bit unsigned, 4 samples packed into 5 bytes (Domesday Duplicator)
    R30,      // 10-bit unsigned, 3 samples packed into a little-endian uint32
    S8,
    U8,
    S16,
    U16,
    S16BE,
    U16BE,
    F32,      // 32-bit float, +-1.0 full scale (ld-decode's .rf)
    Flac,     // native FLAC stream (.flac, .flac.ldf)
    OggFlac,  // FLAC in an Ogg container (.ldf, .raw.oga)
};

struct FormatInfo {
    const char *name;
    int bits;             // code width
    int32_t zero;         // code value that carries no signal
    int32_t min_code;
    int32_t max_code;
    int samples_per_group; // 1 unless the format packs samples across bytes
    int bytes_per_group;   // 0 for the compressed formats
};

const FormatInfo &formatInfo(Format format);

inline bool isFlac(Format format) {
    return format == Format::Flac || format == Format::OggFlac;
}

std::optional<Format> formatFromName(std::string_view name);
std::optional<Format> formatFromFilename(std::string_view path);

// Recognizes the container from the first bytes of a file.  This is the only
// reliable way to tell the two kinds of .ldf apart: ld-compress writes Ogg FLAC
// in -c mode and native FLAC in -a mode, both under the same extension.
std::optional<Format> formatFromMagic(const unsigned char *data, size_t size);

std::string formatNameList();

#endif //LDCONV_FORMAT_H
