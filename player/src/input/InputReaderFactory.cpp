// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <chrono>
#include <cstdint>
#include <format>
#include "ByteSource.h"
#include "EthadcByteSource.h"
#include "InputReaderFactory.h"
#include "LdfInputReader.h"
#include "LdsInputReader.h"
#include "PrefetchingInputReader.h"

static std::unique_ptr<InputReader> makeInputReaderForSource(
        std::unique_ptr<ByteSource> source, InputFormat format, uint32_t block_size) {
    switch (format) {
        case eUint8:    return std::make_unique<InputReaderImpl<uint8_t>>(std::move(source), block_size);
        case eSint8:    return std::make_unique<InputReaderImpl<int8_t>>(std::move(source), block_size);
        case eUint16:   return std::make_unique<InputReaderImpl<uint16_t>>(std::move(source), block_size);
        case eSint16:   return std::make_unique<InputReaderImpl<int16_t>>(std::move(source), block_size);
        case eUint16BE: return std::make_unique<InputReaderImpl<uint16_t, true>>(std::move(source), block_size);
        case eSint16BE: return std::make_unique<InputReaderImpl<int16_t, true>>(std::move(source), block_size);
        case eLds:      return std::make_unique<LdsInputReader>(std::move(source), block_size);
        case eFlac:
        case eFlacOgg:
            // FLAC decoding costs milliseconds per block, too much to leave in series with
            // a real-time consumer's other work, so decode ahead on a separate thread.
            return std::make_unique<PrefetchingInputReader>(
                std::make_unique<LdfInputReader>(std::move(source), block_size, format));
    }
    throw std::runtime_error("Unsupported input format");
}

std::unique_ptr<InputReader> makeInputReader(
    const std::string &name, InputFormat format, uint32_t block_size, Logger *log) {
    return makeInputReaderForSource(openByteSource(name, log), format, block_size);
}

std::unique_ptr<InputReader> makeStdinInputReader(InputFormat format, uint32_t block_size) {
    return makeInputReaderForSource(openStdinByteSource(), format, block_size);
}

std::optional<NetworkStreamInfo> probeNetworkStream(const std::string &name, double timeout_seconds) {
    const auto info = EthadcByteSource::probe(
            name, std::chrono::milliseconds((int64_t)(timeout_seconds * 1000)));
    if (!info)
        return std::nullopt;
    if (info->format_name == "s10p")
        throw std::runtime_error(std::format(
                "{}: the stream carries signed packed 10-bit samples (s10p), which museld cannot read yet",
                name));
    return NetworkStreamInfo{inputFormatFromString(info->format_name), info->sample_frequency};
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
    else if (name == "u10p")  return eLds; // the ethadc stream's name for the same packing
    else if (name == "flac")  return eFlac;
    else if (name == "ldf")   return eFlacOgg;
    else throw std::runtime_error(std::format("Unknown input format: {}", name));
}

const char *inputFormatName(InputFormat format) {
    switch (format) {
        case eUint8:    return "u8";
        case eSint8:    return "s8";
        case eUint16:   return "u16";
        case eSint16:   return "s16";
        case eUint16BE: return "u16be";
        case eSint16BE: return "s16be";
        case eLds:      return "lds";
        case eFlac:     return "flac";
        case eFlacOgg:  return "ldf";
    }
    return "?";
}
