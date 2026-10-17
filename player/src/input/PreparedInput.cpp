// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cerrno>
#include <cstring>
#include <format>
#include <stdexcept>
#include "InputReaderFactory.h"
#include "PreparedInput.h"
#include "RebatchingInputReader.h"

PreparedInput::PreparedInput(std::string name, std::unique_ptr<ByteSource> source,
                             std::optional<InputFormat> format)
    : m_name(std::move(name)),
      m_source(std::move(source)),
      m_format(format),
      m_live(m_source->isLive()) {
}

bool PreparedInput::isLive() const {
    return m_live;
}

ByteSource &PreparedInput::source() {
    if (!m_source)
        throw std::logic_error(std::format("{}: the input has been opened as a reader already", m_name));
    return *m_source;
}

std::unique_ptr<ByteSource> PreparedInput::releaseSource() {
    source(); // checks that it is still here
    return std::move(m_source);
}

void PreparedInput::adoptReader(std::unique_ptr<InputReader> reader, InputFormat format) {
    m_reader = std::move(reader);
    m_source.reset(); // the reader owns it
    m_format = format;
}

std::unique_ptr<InputReader> PreparedInput::open(uint32_t block_size, Logger *log) {
    if (!m_format)
        throw std::logic_error(std::format("{}: the sample format is not known", m_name));
    if (m_reader)
        return std::make_unique<RebatchingInputReader>(std::move(m_reader), block_size);
    if (!m_source)
        throw std::logic_error(std::format("{}: the input has been opened already", m_name));
    if (!m_live && m_source->seek(0, SEEK_SET) != 0)
        throw std::runtime_error(std::format("{}: cannot rewind the input: {}", m_name, strerror(errno)));
    return makeInputReader(std::move(m_source), *m_format, block_size, log);
}
