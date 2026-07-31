// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cstdint>
#include <unistd.h>
#include <cassert>
#include <mutex>
#include <sys/stat.h>
#include "FLAC++/decoder.h"
#include "LdfInputReader.h"

LdfInputReader::LdfInputReader(int fd, uint32_t block_size, bool is_fifo, InputFormat format)
  : InputReader(fd, block_size, is_fifo),
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

    // Perform first processing so we can seek before readFloats is called
    FLAC__StreamDecoderState s;
    while (s = get_state(), s != FLAC__STREAM_DECODER_READ_FRAME && s != FLAC__STREAM_DECODER_SEARCH_FOR_FRAME_SYNC && s != FLAC__STREAM_DECODER_END_OF_STREAM)
        if (!process_single())
            throw std::runtime_error(std::format("libFLAC++ process_single: {}",
                FLAC__StreamDecoderErrorStatusString[get_state()]));
}

void LdfInputReader::seek(off_t no_samples) {
    if (m_is_fifo)
        return;
    std::scoped_lock<std::mutex> lock(m_fd_mutex);
    if (!seek_absolute(m_sample_position + no_samples))
        throw std::runtime_error("libflac: seek_absolute failed");
    m_sample_position += no_samples;
}

int LdfInputReader::readFloats(float *f) {
    std::scoped_lock<std::mutex> lock(m_fd_mutex);
    int filled_floats = 0;
    float dc = dcOffset();
    double sum = 0;
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
        for (int i = 0; i < n; i++) {
            float v = (float)(int16_t)m_decoded_samples[m_flac_block_read_count++];
            sum += v;
            f[filled_floats++] = v - dc;
        }
    }
    updateDc(sum / m_block_size);
    m_sample_position += m_block_size;
    return m_block_size;
}

FLAC__StreamDecoderReadStatus LdfInputReader::read_callback(FLAC__byte buffer[], size_t *bytes) {
    const auto r = read(m_fd, buffer, *bytes);
    if (r == -1)
        throw std::runtime_error(std::format("Error reading from file: {}", strerror(errno)));
    *bytes = r;
    if (r == 0)
        return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;

    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

FLAC__StreamDecoderWriteStatus LdfInputReader::write_callback(const FLAC__Frame *frame, const FLAC__int32 *const buffer[]) {
    // A block shorter than the buffer is not a sign of trouble: a seek that lands inside a
    // frame makes libFLAC hand back only that frame's tail, and the last frame of a stream
    // may be short as well.  Only the capacity matters here.
    if (frame->header.blocksize > m_flac_allocated_size) {
        delete[] m_decoded_samples;
        m_decoded_samples = new uint16_t[frame->header.blocksize];
        m_flac_allocated_size = frame->header.blocksize;
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

FLAC__StreamDecoderTellStatus LdfInputReader::tell_callback(FLAC__uint64 *absolute_byte_offset) {
    *absolute_byte_offset = lseek(m_fd, 0, SEEK_CUR);;
    if (*absolute_byte_offset == -1) {
        if (errno == ESPIPE)
            return FLAC__STREAM_DECODER_TELL_STATUS_UNSUPPORTED;
        return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
    }
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

FLAC__StreamDecoderSeekStatus LdfInputReader::seek_callback(FLAC__uint64 absolute_byte_offset) {
    int r = lseek(m_fd, (off_t)absolute_byte_offset, SEEK_SET);
    if (r == -1) {
        if (errno == ESPIPE)
            return FLAC__STREAM_DECODER_SEEK_STATUS_UNSUPPORTED;
        return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    }
    return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

FLAC__StreamDecoderLengthStatus LdfInputReader::length_callback(FLAC__uint64 *stream_length) {
    struct stat stats{};
    if(fstat(m_fd, &stats) != 0)
        return FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;
    else {
        *stream_length = (FLAC__uint64)stats.st_size;
        return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
    }
}

bool LdfInputReader::eof_callback() {
    return false; // TODO actually check!
}
