// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_PREPAREDINPUT_H
#define AC3RF_DECODE_PREPAREDINPUT_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include "ByteSource.h"
#include "InputReader.h"

class Logger;

// An input opened once and handed from the content probe to the player.  A
// file is opened, probed at a few positions, rewound and handed over.  A live
// input -- a fifo, a pipe on stdin, the network stream -- cannot be reopened
// at the point it has reached, so it has to be the same open source that the
// probe read from and the player then decodes; and where the probe had to
// decode the stream to look at it (FLAC, whose decoder cannot restart in the
// middle of a stream), the player takes over the reader itself.
class PreparedInput {
public:
    PreparedInput(std::string name, std::unique_ptr<ByteSource> source,
                  std::optional<InputFormat> format = std::nullopt);
    PreparedInput(PreparedInput &&) noexcept = default;
    PreparedInput &operator=(PreparedInput &&) noexcept = default;

    const std::string &name() const { return m_name; }
    bool isLive() const;
    std::optional<InputFormat> format() const { return m_format; }
    void setFormat(InputFormat format) { m_format = format; }

    // For the probe: the source as long as it has not been opened as a reader.
    ByteSource &source();
    // For the probe: takes the source, to decode a live input from here on
    // with a reader it then hands back through adoptReader().
    std::unique_ptr<ByteSource> releaseSource();
    // For the probe: the reader it decoded a live stream with, which the
    // player continues from.  Its format becomes the input's.
    void adoptReader(std::unique_ptr<InputReader> reader, InputFormat format);

    // The reader the player decodes with, in blocks of `block_size` samples;
    // the format must be known by now.  Once only: the reader owns the source
    // from here.  A seekable source is rewound first.
    std::unique_ptr<InputReader> open(uint32_t block_size, Logger *log);

private:
    std::string m_name;
    std::unique_ptr<ByteSource> m_source;
    std::unique_ptr<InputReader> m_reader;
    std::optional<InputFormat> m_format;
    bool m_live;
};

#endif //AC3RF_DECODE_PREPAREDINPUT_H
