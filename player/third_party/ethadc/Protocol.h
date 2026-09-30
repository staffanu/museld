// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ethadc UDP sample-stream wire format.
//
// This header is the specification the FPGA has to match, so everything in it
// is fixed-size, naturally aligned and little endian: no length prefixes, no
// computed fields, nothing that needs more than a counter and a register read
// on the sender side.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ethadc {

inline constexpr std::uint16_t kMagic = 0xE7AD;
inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::size_t kHeaderSize = 16;

// Set on the first packet of a stream, and again whenever the sender restarts
// its sequence counter.  The receiver resynchronises instead of treating the
// jump as a huge loss.
inline constexpr std::uint16_t kFlagRestart = 0x0001;

// Set on the last packet of a stream, which is the only one allowed to carry a
// short payload.  Every other packet must be exactly as long as the first, or
// the receiver could not turn a sequence gap into a byte count.  A continuously
// streaming sender -- the FPGA -- never sets this.
inline constexpr std::uint16_t kFlagEnd = 0x0002;

// Every payload is a whole number of sample groups so that no sample straddles
// a packet boundary: 1 byte for the 8-bit formats, 2 for the 16-bit ones, 5 for
// the packed 10-bit ones.  A multiple of 10 satisfies all three.
inline constexpr std::size_t kPayloadGranule = 10;

// 16 + 1440 = 1456 bytes of UDP, 1484 on the wire: fits a 1500-byte MTU.
inline constexpr std::size_t kDefaultPayload = 1440;
// 16 + 8950 = 8966, against the 8972 a 9000-byte MTU allows.
inline constexpr std::size_t kJumboPayload = 8950;

inline constexpr std::size_t kMaxPayload = 65507 - kHeaderSize;

// The container the samples arrive in.  The names match museld's
// --input-format so a stream can be handed straight to it.
enum class Format : std::uint8_t {
    U8 = 0,     // unsigned 8-bit, offset binary
    S8 = 1,     // signed 8-bit, two's complement
    U16LE = 2,  // unsigned 16-bit little endian
    S16LE = 3,  // signed 16-bit little endian
    U10P = 4,   // unsigned 10-bit, 4 samples packed into 5 bytes: the .lds packing
    S10P = 5,   // signed 10-bit, 4 samples packed into 5 bytes, same layout
};

// The packed 10-bit layout, spelled out because the FPGA must reproduce it
// bit for bit.  It is the Domesday Duplicator's .lds packing, so a u10p stream
// written to a file *is* an .lds file and museld's lds reader consumes it
// unchanged.  Samples s0..s3 become bytes b0..b4, most significant bits first:
//
//   b0 = s0[9:2]
//   b1 = s0[1:0] << 6 | s1[9:4]
//   b2 = s1[3:0] << 4 | s2[9:6]
//   b3 = s2[5:0] << 2 | s3[9:8]
//   b4 = s3[7:0]
//
// pack_10bit and unpack_10bit are that rule as code, for tests and converters.
// Signed samples (s10p) go through the same layout as their 10-bit two's
// complement pattern.
inline constexpr void pack_10bit(const std::uint16_t s[4], std::uint8_t b[5]) {
    b[0] = static_cast<std::uint8_t>(s[0] >> 2);
    b[1] = static_cast<std::uint8_t>((s[0] & 0x3) << 6 | (s[1] >> 4));
    b[2] = static_cast<std::uint8_t>((s[1] & 0xf) << 4 | (s[2] >> 6));
    b[3] = static_cast<std::uint8_t>((s[2] & 0x3f) << 2 | (s[3] >> 8));
    b[4] = static_cast<std::uint8_t>(s[3] & 0xff);
}

inline constexpr void unpack_10bit(const std::uint8_t b[5], std::uint16_t s[4]) {
    s[0] = static_cast<std::uint16_t>(b[0] << 2 | b[1] >> 6);
    s[1] = static_cast<std::uint16_t>((b[1] & 0x3f) << 4 | b[2] >> 4);
    s[2] = static_cast<std::uint16_t>((b[2] & 0x0f) << 6 | b[3] >> 2);
    s[3] = static_cast<std::uint16_t>((b[3] & 0x03) << 8 | b[4]);
}

struct FormatInfo {
    const char *name;
    std::size_t group_bytes;    // bytes in the smallest whole-sample unit
    std::size_t group_samples;  // samples in that unit
    bool is_signed;
};

inline constexpr FormatInfo kFormatInfo[] = {
    {"u8", 1, 1, false},   {"s8", 1, 1, true},     {"u16", 2, 1, false},
    {"s16", 2, 1, true},   {"u10p", 5, 4, false},  {"s10p", 5, 4, true},
};

inline constexpr bool format_valid(std::uint8_t f) {
    return f < std::size(kFormatInfo);
}

inline constexpr const FormatInfo &format_info(Format f) {
    return kFormatInfo[static_cast<std::uint8_t>(f)];
}

inline std::optional<Format> format_from_name(std::string_view name) {
    for (std::size_t i = 0; i < std::size(kFormatInfo); ++i)
        if (name == kFormatInfo[i].name)
            return static_cast<Format>(i);
    return std::nullopt;
}

inline std::string format_names() {
    std::string s;
    for (const auto &info : kFormatInfo) {
        if (!s.empty())
            s += '|';
        s += info.name;
    }
    return s;
}

// Bytes per sample as an exact ratio, so that pacing arithmetic stays integral
// for the 1.25 bytes/sample packed formats.
inline constexpr void bytes_per_sample(Format f, std::uint64_t &num, std::uint64_t &den) {
    num = format_info(f).group_bytes;
    den = format_info(f).group_samples;
}

// One sample group holding the format's idea of zero signal.  Signed formats
// centre on 0; the unsigned ones on the middle of their range.  For 10-bit data
// carried in a 16-bit container the true centre is 512 rather than 32768, which
// is why the receiver defaults to holding the last sample rather than to this.
inline std::vector<std::uint8_t> midscale_group(Format f) {
    switch (f) {
        case Format::U8: return {0x80};
        case Format::S8: return {0x00};
        case Format::U16LE: return {0x00, 0x80};
        case Format::S16LE: return {0x00, 0x00};
        case Format::U10P: return {0x80, 0x20, 0x08, 0x02, 0x00};  // 512 x 4
        case Format::S10P: return {0x00, 0x00, 0x00, 0x00, 0x00};
    }
    return {0x00};
}

struct Header {
    std::uint16_t magic = kMagic;
    std::uint8_t version = kVersion;
    Format format = Format::U8;
    std::uint32_t seq = 0;
    std::uint32_t sample_rate = 0;  // Hz, constant for the life of a stream
    std::uint16_t payload_len = 0;  // bytes of samples following the header
    std::uint16_t flags = 0;
};

inline void put_u16(std::uint8_t *p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}

inline void put_u32(std::uint8_t *p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

inline std::uint16_t get_u16(const std::uint8_t *p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

inline std::uint32_t get_u32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

inline void encode_header(const Header &h, std::uint8_t *p) {
    put_u16(p + 0, h.magic);
    p[2] = h.version;
    p[3] = static_cast<std::uint8_t>(h.format);
    put_u32(p + 4, h.seq);
    put_u32(p + 8, h.sample_rate);
    put_u16(p + 12, h.payload_len);
    put_u16(p + 14, h.flags);
}

// Validates everything the receiver would otherwise have to trust: a stray
// packet on the port must not be able to steer the reorder window.
inline bool decode_header(const std::uint8_t *p, std::size_t len, Header &h) {
    if (len < kHeaderSize)
        return false;
    h.magic = get_u16(p + 0);
    h.version = p[2];
    if (h.magic != kMagic || h.version != kVersion)
        return false;
    if (!format_valid(p[3]))
        return false;
    h.format = static_cast<Format>(p[3]);
    h.seq = get_u32(p + 4);
    h.sample_rate = get_u32(p + 8);
    h.payload_len = get_u16(p + 12);
    h.flags = get_u16(p + 14);
    if (h.payload_len == 0 || len != kHeaderSize + h.payload_len)
        return false;
    if (h.payload_len % format_info(h.format).group_bytes != 0)
        return false;
    return true;
}

}  // namespace ethadc
