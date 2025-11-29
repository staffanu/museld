//
// Created by Staffan Ulfberg on 10/12/25.
//

#ifndef AC3RF_DECODE_LDFINPUTREADER_H
#define AC3RF_DECODE_LDFINPUTREADER_H

#include "InputReader.h"
#include <FLAC++/decoder.h>

class LdfInputReader : public InputReader, private FLAC::Decoder::Stream  {
public:
    LdfInputReader(int fd, uint32_t block_size, InputFormat format);

    LdfInputReader(const LdfInputReader &) = delete;
    LdfInputReader &operator=(const LdfInputReader &) = delete;
    LdfInputReader(LdfInputReader &&) = delete;
    LdfInputReader &operator=(LdfInputReader &&) = delete;

    ~LdfInputReader() override;

    void initialize() override;
    int readFloats(float *f) override;

private:
    FLAC__StreamDecoderReadStatus read_callback(FLAC__byte buffer[], size_t *bytes) override;
    FLAC__StreamDecoderWriteStatus write_callback(const ::FLAC__Frame *frame, const FLAC__int32 *const buffer[]) override;
    void metadata_callback(const ::FLAC__StreamMetadata *metadata) override;
    void error_callback(::FLAC__StreamDecoderErrorStatus status) override;

    InputFormat m_format;
    int m_bits_per_sample = 0;
    uint32_t m_flac_allocated_size = 0;
    uint32_t m_flac_used_size = 0;
    uint32_t m_flac_block_read_count = 0;
    uint16_t *m_decoded_samples = nullptr;
};

#endif //AC3RF_DECODE_LDFINPUTREADER_H
