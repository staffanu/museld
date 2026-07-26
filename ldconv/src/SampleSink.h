// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef LDCONV_SAMPLESINK_H
#define LDCONV_SAMPLESINK_H

#include <cstdint>
#include <memory>
#include <string>
#include "Format.h"

struct SinkOptions {
    int flac_level = 8;
    unsigned flac_blocksize = 4096;
    // 0 leaves the predictor order that the compression level picked.  Raising
    // it to 32 takes another 4% or so off an RF capture, for several times the
    // encoding work; the README has the measurements.
    unsigned flac_lpc_order = 0;
    // FLAC tops out at 655350 Hz, so a capture rate cannot be stored as it is.
    // ld-compress writes the rate in kHz (40 MHz becomes 40000) and this
    // follows that, which keeps the files interchangeable with ld-decode's.
    unsigned flac_rate = 40000;
    unsigned threads = 0;       // 0 asks for one thread per core
    uint64_t total_samples = 0; // 0 when the length is not known in advance
};

// Takes code values already scaled into this format's domain and writes them.
class SampleSink {
public:
    virtual ~SampleSink() = default;

    SampleSink(const SampleSink &) = delete;
    SampleSink &operator=(const SampleSink &) = delete;

    virtual void write(const int32_t *in, size_t count) = 0;

    // Flushes and closes; must be called for the output to be complete.
    virtual void close() = 0;

    virtual int bits() const { return formatInfo(m_format).bits; }
    virtual int32_t zero() const { return formatInfo(m_format).zero; }

    Format format() const { return m_format; }

protected:
    explicit SampleSink(Format format) : m_format(format) {}

    Format m_format;
};

// Creates path ("-" for stdout).  The caller has already decided the format;
// there is no file to sniff.
std::unique_ptr<SampleSink> makeSink(const std::string &path, Format format, const SinkOptions &options);

#endif //LDCONV_SAMPLESINK_H
