// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_INPUTREADERFACTORY_H
#define AC3RF_DECODE_INPUTREADERFACTORY_H

#include <memory>
#include <optional>
#include <string>
#include "InputReader.h"

// Open `filename` and return an InputReader for `format`. The fd is owned by the returned reader.
// On Linux, if the file is a fifo the pipe buffer is bumped to 1 MiB.
std::unique_ptr<InputReader> makeInputReader(
    const std::string &filename, InputFormat format, uint32_t block_size);

// Return an InputReader for STDIN_FILENO. Seeking is silently ignored.
std::unique_ptr<InputReader> makeStdinInputReader(InputFormat format, uint32_t block_size);

// Match the well-known input file extensions to a format. Returns nullopt for unknown extensions.
std::optional<InputFormat> inputFormatFromFilename(const std::string &filename);

// Map a format name (u8, s8, u16, s16, u16be, s16be, lds, flac, ldf) to an InputFormat.
// Throws std::runtime_error for unknown names.
InputFormat inputFormatFromString(const std::string &name);

#endif //AC3RF_DECODE_INPUTREADERFACTORY_H
