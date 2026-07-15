// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_LDFINPUTREADER_H
#define AC3RF_DECODE_LDFINPUTREADER_H

#include "InputReader.h"
#include <FLAC++/decoder.h>

class LdfInputReader : public InputReader, private FLAC::Decoder::Stream  {
public:
    LdfInputReader(int fd, uint32_t block_size, bool is_fifo, InputFormat format);

    LdfInputReader(const LdfInputReader &) = delete;
    LdfInputReader &operator=(const LdfInputReader &) = delete;
    LdfInputReader(LdfInputReader &&) = delete;
    LdfInputReader &operator=(LdfInputReader &&) = delete;

    ~LdfInputReader() override;

    void initialize() override;
    void seek(off_t no_samples) override;
    int readFloats(float *f) override;
    int bitsPerSample() const override { return m_bits_per_sample; }

private:
    FLAC__StreamDecoderReadStatus read_callback(FLAC__byte buffer[], size_t *bytes) override;
    FLAC__StreamDecoderWriteStatus write_callback(const ::FLAC__Frame *frame, const FLAC__int32 *const buffer[]) override;
    void metadata_callback(const ::FLAC__StreamMetadata *metadata) override;
    void error_callback(::FLAC__StreamDecoderErrorStatus status) override;
    FLAC__StreamDecoderTellStatus tell_callback(FLAC__uint64 *absolute_byte_offset) override;
    FLAC__StreamDecoderSeekStatus seek_callback(FLAC__uint64 absolute_byte_offset) override;
    FLAC__StreamDecoderLengthStatus length_callback(FLAC__uint64 *stream_length) override;
    bool eof_callback() override;

    InputFormat m_format;
    int m_bits_per_sample = 0;
    uint32_t m_flac_allocated_size = 0;
    uint32_t m_flac_used_size = 0;
    uint32_t m_flac_block_read_count = 0;
    uint16_t *m_decoded_samples = nullptr;
    uint64_t m_sample_position = 0;
};

#endif //AC3RF_DECODE_LDFINPUTREADER_H
