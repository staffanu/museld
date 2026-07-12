// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#include <fcntl.h>
#include <unistd.h>
#include <filesystem>
#include <format>
#include "InputReaderFactory.h"
#include "LdfInputReader.h"
#include "LdsInputReader.h"

#ifndef O_BINARY
#  define O_BINARY 0
#endif
// Windows has no FIFOs; is_fifo() is always false there, so the flag is never used.
#ifndef O_NONBLOCK
#  define O_NONBLOCK 0
#endif

static std::unique_ptr<InputReader> makeInputReaderForFd(int fd, bool is_fifo, InputFormat format, uint32_t block_size) {
    switch (format) {
        case eUint8:    return std::make_unique<InputReaderImpl<uint8_t>>(fd, block_size, is_fifo);
        case eSint8:    return std::make_unique<InputReaderImpl<int8_t>>(fd, block_size, is_fifo);
        case eUint16:   return std::make_unique<InputReaderImpl<uint16_t>>(fd, block_size, is_fifo);
        case eSint16:   return std::make_unique<InputReaderImpl<int16_t>>(fd, block_size, is_fifo);
        case eUint16BE: return std::make_unique<InputReaderImpl<uint16_t, true>>(fd, block_size, is_fifo);
        case eSint16BE: return std::make_unique<InputReaderImpl<int16_t, true>>(fd, block_size, is_fifo);
        case eLds:     return std::make_unique<LdsInputReader>(fd, block_size, is_fifo);
        case eFlac:
        case eFlacOgg: return std::make_unique<LdfInputReader>(fd, block_size, is_fifo, format);
    }
    throw std::runtime_error("Unsupported input format");
}

std::unique_ptr<InputReader> makeInputReader(
    const std::string &filename, InputFormat format, uint32_t block_size) {

    bool is_fifo = std::filesystem::is_fifo(filename);
    int flags = O_RDONLY | O_BINARY;
    if (is_fifo)
        flags |= O_NONBLOCK;

    int fd = open(filename.c_str(), flags);
    if (fd == -1)
        throw std::runtime_error(std::format("Unable to open input file {}: {}", filename, strerror(errno)));

#ifdef linux
    if (is_fifo) {
        fcntl(fd, F_SETPIPE_SZ, 1024 * 1024);
    }
#endif

    return makeInputReaderForFd(fd, is_fifo, format, block_size);
}

std::unique_ptr<InputReader> makeStdinInputReader(InputFormat format, uint32_t block_size) {
    return makeInputReaderForFd(STDIN_FILENO, true, format, block_size);
}

std::optional<InputFormat> inputFormatFromFilename(const std::string &filename) {
    if (filename.ends_with(".u8"))    return eUint8;
    if (filename.ends_with(".s8"))    return eSint8;
    if (filename.ends_with(".u16"))   return eUint16;
    if (filename.ends_with(".s16"))   return eSint16;
    if (filename.ends_with(".u16be")) return eUint16BE;
    if (filename.ends_with(".s16be")) return eSint16BE;
    if (filename.ends_with(".lds"))   return eLds;
    if (filename.ends_with(".flac"))  return eFlac;
    if (filename.ends_with(".flac.ldf")) return eFlac;
    if (filename.ends_with(".ldf"))      return eFlacOgg;
    return std::nullopt;
}

InputFormat inputFormatFromString(const std::string &name) {
    if      (name == "u8")    return eUint8;
    else if (name == "s8")    return eSint8;
    else if (name == "u16")   return eUint16;
    else if (name == "s16")   return eSint16;
    else if (name == "u16be") return eUint16BE;
    else if (name == "s16be") return eSint16BE;
    else if (name == "lds")   return eLds;
    else if (name == "flac")  return eFlac;
    else if (name == "ldf")   return eFlacOgg;
    else throw std::runtime_error(std::format("Unknown input format: {}", name));
}
