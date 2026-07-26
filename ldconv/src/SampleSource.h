// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef LDCONV_SAMPLESOURCE_H
#define LDCONV_SAMPLESOURCE_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include "Format.h"

// Produces the raw code values of a capture, one int32 per sample, whatever the
// container did to them.  Scaling into the destination's domain happens later,
// in one pass, so nothing is rounded twice.
class SampleSource {
public:
    virtual ~SampleSource() = default;

    SampleSource(const SampleSource &) = delete;
    SampleSource &operator=(const SampleSource &) = delete;

    // Fills up to count samples; a short count means the input ended.
    virtual size_t read(int32_t *out, size_t count) = 0;

    // Discards the first count samples.  Seeks when the stream allows it and
    // reads past them when it does not, so this works on a pipe too.
    virtual void skip(uint64_t count) = 0;

    // 0 when the length is not known ahead of time (pipes, and FLAC streams
    // whose header does not carry a sample count).
    virtual uint64_t totalSamples() const { return 0; }

    // How far through the file the reading has got, in bytes, and how big it
    // is; -1 for either when there is nothing to measure.  This is what a
    // progress report has to fall back on for the ld-compress output, which
    // was written through a pipe and so declares no sample count.
    virtual int64_t bytePosition() const { return -1; }
    virtual int64_t byteSize() const { return -1; }

    // The code width actually in use, which for FLAC comes from the stream
    // header rather than from the extension.
    virtual int bits() const { return formatInfo(m_format).bits; }
    virtual int32_t zero() const { return formatInfo(m_format).zero; }

    // Extra lines for --info; empty for the formats that carry no header.
    virtual std::string description() const { return {}; }

    Format format() const { return m_format; }

protected:
    explicit SampleSource(Format format) : m_format(format) {}

    Format m_format;
};

// Opens path ("-" for stdin).  Without an explicit format the container is
// taken from the file's magic bytes where the file is seekable, and from the
// extension otherwise.
std::unique_ptr<SampleSource> makeSource(const std::string &path, std::optional<Format> forced_format);

#endif //LDCONV_SAMPLESOURCE_H
