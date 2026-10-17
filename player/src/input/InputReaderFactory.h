// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_INPUTREADERFACTORY_H
#define AC3RF_DECODE_INPUTREADERFACTORY_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include "InputReader.h"

class Logger;

// Open the input `name` -- a file, a fifo, "-" for stdin or a URL (see ByteSource.h) -- and
// return an InputReader for `format`.  On Linux, if the file is a fifo the pipe buffer is
// bumped to 1 MiB.  `log` may be null; the network sources report on themselves through it.
std::unique_ptr<InputReader> makeInputReader(
    const std::string &name, InputFormat format, uint32_t block_size, Logger *log = nullptr);

// The same over a source that is already open; the reader owns it from here.
std::unique_ptr<InputReader> makeInputReader(
    std::unique_ptr<ByteSource> source, InputFormat format, uint32_t block_size, Logger *log = nullptr);

// What a udp:// stream's packets say about it: the sample format and rate ride in every
// packet's header.  Waits up to `timeout_seconds` for the first one on an open udp://
// source; nullopt if none came, or if `source` is not such a stream.  Throws
// std::runtime_error when the stream carries a format museld cannot read.
struct NetworkStreamInfo {
    InputFormat format;
    double sample_frequency;
};
std::optional<NetworkStreamInfo> networkStreamInfo(ByteSource &source, double timeout_seconds);

// Match the well-known input file extensions to a format. Returns nullopt for unknown extensions.
std::optional<InputFormat> inputFormatFromFilename(const std::string &filename);

// Map a format name (u8, s8, u16, s16, u16be, s16be, lds, flac, ldf; u10p is the ethadc
// stream's name for the lds packing) to an InputFormat.
// Throws std::runtime_error for unknown names.
InputFormat inputFormatFromString(const std::string &name);

// The inverse: the name inputFormatFromString accepts for this format.
const char *inputFormatName(InputFormat format);

#endif //AC3RF_DECODE_INPUTREADERFACTORY_H
