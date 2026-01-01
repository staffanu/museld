//
// Created by Staffan Ulfberg on 10/12/25.
//

#include <unistd.h>
#include <cassert>
#include "FLAC++/decoder.h"
#include "LdfInputReader.h"

LdfInputReader::LdfInputReader(int fd, uint32_t block_size, InputFormat format)
  : InputReader(fd, block_size),
    FLAC::Decoder::Stream(),
    m_format(format) {
    assert(format == eFlacOgg || format == eFlac);
}

LdfInputReader::~LdfInputReader() {
    delete[] m_decoded_samples;
}

void LdfInputReader::initialize() {
    auto status = m_format == eFlac ? init() : init_ogg();
    if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK)
        throw std::runtime_error(std::format("Error initializing decoder: {}", FLAC__StreamDecoderInitStatusString[status]));
}

void LdfInputReader::seek(off_t no_samples) {
    throw std::runtime_error("LDF files cannot be seeked");
}


int LdfInputReader::readFloats(float *f) {
    int filled_floats = 0;
    while (filled_floats < m_block_size) {
        if (m_flac_block_read_count == m_flac_used_size) {
            // notice this triggers also the first time, when both are zero
            if (!process_single()) {
                throw std::runtime_error(std::format("libFLAC++ process_single: {}",
                    FLAC__StreamDecoderErrorStatusString[get_state()]));
            } else if (get_state() == FLAC__STREAM_DECODER_END_OF_STREAM) {
                return 0;
            }
        }
        int n = std::min(m_block_size - filled_floats, m_flac_used_size - m_flac_block_read_count);
        for (int i = 0; i < n; i++)
            f[filled_floats++] = (int16_t)m_decoded_samples[m_flac_block_read_count++];
    }
    return m_block_size;
}

FLAC__StreamDecoderReadStatus LdfInputReader::read_callback(FLAC__byte buffer[], size_t *bytes) {
    const auto r = read(m_fd, buffer, *bytes);
    if (r == -1)
        throw std::runtime_error(std::format("Error reading from file: {}", strerror(errno)));
    *bytes = r;
    if (r == 0)
        return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    else
        return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

FLAC__StreamDecoderWriteStatus LdfInputReader::write_callback(const FLAC__Frame *frame, const FLAC__int32 *const buffer[]) {
    if (m_decoded_samples == nullptr) {
        m_decoded_samples = new uint16_t[frame->header.blocksize];
        m_flac_allocated_size = frame->header.blocksize;
    } else if ( frame->header.blocksize > m_flac_allocated_size) {
        throw std::runtime_error("LDF block size increased within file");
    }

    for (int i = 0; i < frame->header.blocksize; i++)
        m_decoded_samples[i] = buffer[0][i];
    m_flac_used_size = frame->header.blocksize;
    m_flac_block_read_count = 0;

    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void LdfInputReader::metadata_callback(const ::FLAC__StreamMetadata *metadata) {
    if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
        if (metadata->data.stream_info.channels != 1)
            throw std::runtime_error("LDF files should have only one channel");
        if (metadata->data.stream_info.bits_per_sample == 8 || metadata->data.stream_info.bits_per_sample == 16)
            m_bits_per_sample = metadata->data.stream_info.bits_per_sample;
        else
            throw std::runtime_error("LDF files should have 8 or 16 bits per sample");
    }
}

void LdfInputReader::error_callback(::FLAC__StreamDecoderErrorStatus status) {
    throw std::runtime_error(std::format("libFLAC++ error_callback: {}", FLAC__StreamDecoderErrorStatusString[status]));
}
