// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <cstdint>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>
#include <cstring>
#include "FLAC++/decoder.h"
#include "LdfInputReader.h"
#include "logging/Logger.h"

LdfInputReader::LdfInputReader(std::unique_ptr<ByteSource> source, uint32_t block_size, InputFormat format,
                               Logger *log)
  : InputReader(std::move(source), block_size),
    FLAC::Decoder::Stream(),
    m_format(format),
    m_log(log) {
    assert(format == eFlacOgg || format == eFlac);
}

LdfInputReader::~LdfInputReader() {
    delete[] m_decoded_samples;
}

void LdfInputReader::recordError(std::string message) {
    if (m_failure.empty())
        m_failure = std::move(message);
}

void LdfInputReader::throwIfFailed() const {
    if (!m_failure.empty())
        throw std::runtime_error(m_failure);
}

// Runs one decoding step and reports a callback failure, a step that failed without one, or
// nothing at all.  Every use of process_single goes through here.
void LdfInputReader::processSingleChecked() {
    const bool ok = process_single();
    // A recorded failure names the actual cause, so it wins over libFLAC's resulting state.
    throwIfFailed();
    if (!ok)
        throw std::runtime_error(std::format("libFLAC++ process_single: {}",
            FLAC__StreamDecoderStateString[get_state()]));
}

// Reads the stream's signature to tell plain FLAC ("fLaC") from FLAC in Ogg ("OggS").
// The bytes are kept for read_callback, so this also works on a fifo or a network stream
// where they cannot be read twice.  Returns the format hint unchanged when the stream
// starts with neither.
InputFormat LdfInputReader::detectContainer() {
    while (m_pushback_size < sizeof m_pushback) {
        ssize_t r = m_source->read(m_pushback + m_pushback_size, sizeof m_pushback - m_pushback_size);
        if (r == -1 && errno == EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (r == -1)
            throw std::runtime_error(std::format("Error reading input: {}", strerror(errno)));
        if (r == 0)
            break; // the stream ends inside the signature; libFLAC gets to report that
        m_pushback_size += r;
    }
    if (m_pushback_size < sizeof m_pushback)
        return m_format;
    std::optional<InputFormat> detected;
    if (memcmp(m_pushback, "fLaC", 4) == 0)
        detected = eFlac;
    else if (memcmp(m_pushback, "OggS", 4) == 0)
        detected = eFlacOgg;
    if (!detected)
        return m_format;
    if (*detected != m_format && m_log)
        m_log->info(eInput, std::format("The input is {}, not {} as its name suggests",
            *detected == eFlac ? "plain FLAC" : "FLAC in Ogg", m_format == eFlac ? "plain FLAC" : "FLAC in Ogg"));
    return *detected;
}

void LdfInputReader::initialize() {
    auto status = detectContainer() == eFlac ? init() : init_ogg();
    if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK)
        throw std::runtime_error(std::format("Error initializing decoder: {}", FLAC__StreamDecoderInitStatusString[status]));

    // Perform first processing so we can seek before readFloats is called
    FLAC__StreamDecoderState s;
    while (s = get_state(), s != FLAC__STREAM_DECODER_READ_FRAME && s != FLAC__STREAM_DECODER_SEARCH_FOR_FRAME_SYNC && s != FLAC__STREAM_DECODER_END_OF_STREAM)
        processSingleChecked();
}

void LdfInputReader::seek(int64_t no_samples) {
    if (isLive())
        return;
    std::scoped_lock<std::mutex> lock(m_source_mutex);
    throwIfFailed();
    // Clamp at the start of the stream (see InputReaderImpl::seek)
    no_samples = std::max(no_samples, -(int64_t)m_sample_position);
    const bool ok = seek_absolute(m_sample_position + no_samples);
    throwIfFailed();
    if (!ok)
        throw std::runtime_error("libflac: seek_absolute failed");
    m_sample_position += no_samples;
}

int LdfInputReader::readFloats(float *f) {
    std::scoped_lock<std::mutex> lock(m_source_mutex);
    throwIfFailed();
    int filled_floats = 0;
    float dc = dcOffset();
    double sum = 0;
    while (filled_floats < m_block_size) {
        if (m_flac_block_read_count == m_flac_used_size) {
            // notice this triggers also the first time, when both are zero
            processSingleChecked();
            if (get_state() == FLAC__STREAM_DECODER_END_OF_STREAM)
                return 0;
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
    if (m_pushback_read < m_pushback_size) {
        const size_t n = std::min(*bytes, m_pushback_size - m_pushback_read);
        memcpy(buffer, m_pushback + m_pushback_read, n);
        m_pushback_read += n;
        *bytes = n;
        return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }
    // libFLAC has no "try again later": a live source that has nothing yet is
    // waited for here, as the raw readers do in readFully.
    ssize_t r;
    while ((r = m_source->read(buffer, *bytes)) == -1 && errno == EAGAIN)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (r == -1) {
        recordError(std::format("Error reading input: {}", strerror(errno)));
        *bytes = 0;
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }
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
            recordError("LDF files should have only one channel");
        if (metadata->data.stream_info.bits_per_sample == 8 || metadata->data.stream_info.bits_per_sample == 16)
            m_bits_per_sample = metadata->data.stream_info.bits_per_sample;
        else
            recordError("LDF files should have 8 or 16 bits per sample");
    }
}

void LdfInputReader::error_callback(::FLAC__StreamDecoderErrorStatus status) {
    recordError(std::format("libFLAC++ error_callback: {}", FLAC__StreamDecoderErrorStatusString[status]));
}

FLAC__StreamDecoderTellStatus LdfInputReader::tell_callback(FLAC__uint64 *absolute_byte_offset) {
    const int64_t position = m_source->seek(0, SEEK_CUR);
    if (position == -1) {
        if (errno == ESPIPE)
            return FLAC__STREAM_DECODER_TELL_STATUS_UNSUPPORTED;
        return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
    }
    *absolute_byte_offset = (FLAC__uint64)position - (m_pushback_size - m_pushback_read);
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

FLAC__StreamDecoderSeekStatus LdfInputReader::seek_callback(FLAC__uint64 absolute_byte_offset) {
    int64_t r = m_source->seek((int64_t)absolute_byte_offset, SEEK_SET);
    if (r == -1) {
        if (errno == ESPIPE)
            return FLAC__STREAM_DECODER_SEEK_STATUS_UNSUPPORTED;
        return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    }
    m_pushback_read = m_pushback_size; // the source is positioned explicitly from here on
    return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

FLAC__StreamDecoderLengthStatus LdfInputReader::length_callback(FLAC__uint64 *stream_length) {
    const int64_t size = m_source->size();
    if (size < 0)
        return FLAC__STREAM_DECODER_LENGTH_STATUS_UNSUPPORTED;
    *stream_length = (FLAC__uint64)size;
    return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

bool LdfInputReader::eof_callback() {
    return false; // TODO actually check!
}
