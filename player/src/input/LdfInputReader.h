// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_LDFINPUTREADER_H
#define AC3RF_DECODE_LDFINPUTREADER_H

#include <cstdint>
#include <string>
#include "InputReader.h"
#include <FLAC++/decoder.h>

class Logger;

// Reads FLAC-compressed captures, in a plain FLAC stream or wrapped in Ogg.  The
// container is read from the first bytes of the stream: the extensions do not tell
// them apart reliably (plain FLAC .ldf files exist alongside the usual Ogg ones), so
// `format` only decides the case where the stream starts with neither signature.
class LdfInputReader : public InputReader, private FLAC::Decoder::Stream  {
public:
    LdfInputReader(std::unique_ptr<ByteSource> source, uint32_t block_size, InputFormat format,
                   Logger *log = nullptr);

    LdfInputReader(const LdfInputReader &) = delete;
    LdfInputReader &operator=(const LdfInputReader &) = delete;
    LdfInputReader(LdfInputReader &&) = delete;
    LdfInputReader &operator=(LdfInputReader &&) = delete;

    ~LdfInputReader() override;

    void initialize() override;
    void seek(int64_t no_samples) override;
    int readFloats(float *f) override;
    int bitsPerSample() const override { return m_bits_per_sample; }
    bool signedSamples() const override { return true; } // FLAC samples always are

private:
    // libFLAC invokes the callbacks below from its own C frames, which must not be unwound
    // through: an exception thrown there would skip libFLAC's bookkeeping and leave the
    // decoder inconsistent, and whether it can unwind at all depends on how libFLAC was
    // built.  A failing callback records the reason and asks libFLAC to stop, and the public
    // methods raise it once control is back in C++.  The record is sticky -- the stream is
    // not usable after a failure, so every later call reports the same reason.
    void recordError(std::string message);
    void throwIfFailed() const;
    void processSingleChecked();
    InputFormat detectContainer();

    FLAC__StreamDecoderReadStatus read_callback(FLAC__byte buffer[], size_t *bytes) override;
    FLAC__StreamDecoderWriteStatus write_callback(const ::FLAC__Frame *frame, const FLAC__int32 *const buffer[]) override;
    void metadata_callback(const ::FLAC__StreamMetadata *metadata) override;
    void error_callback(::FLAC__StreamDecoderErrorStatus status) override;
    FLAC__StreamDecoderTellStatus tell_callback(FLAC__uint64 *absolute_byte_offset) override;
    FLAC__StreamDecoderSeekStatus seek_callback(FLAC__uint64 absolute_byte_offset) override;
    FLAC__StreamDecoderLengthStatus length_callback(FLAC__uint64 *stream_length) override;
    bool eof_callback() override;

    InputFormat m_format;
    Logger *m_log;
    // The signature bytes read ahead of libFLAC by initialize(), handed back through
    // read_callback before anything further is read from the source
    uint8_t m_pushback[4] = {};
    size_t m_pushback_size = 0;
    size_t m_pushback_read = 0;
    int m_bits_per_sample = 0;
    uint32_t m_flac_allocated_size = 0;
    uint32_t m_flac_used_size = 0;
    uint32_t m_flac_block_read_count = 0;
    uint16_t *m_decoded_samples = nullptr;
    uint64_t m_sample_position = 0;
    std::string m_failure; // empty until a callback fails; see recordError
};

#endif //AC3RF_DECODE_LDFINPUTREADER_H
