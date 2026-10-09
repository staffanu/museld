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
#include <vector>
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
void LdfInputReader::fillPushback(size_t wanted) {
    assert(wanted <= sizeof m_pushback);
    while (m_pushback_size < wanted) {
        ssize_t r = m_source->read(m_pushback + m_pushback_size, wanted - m_pushback_size);
        if (r == -1 && errno == EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (r == -1)
            throw std::runtime_error(std::format("Error reading input: {}", strerror(errno)));
        if (r == 0)
            break; // the stream ends early; libFLAC gets to report that
        m_pushback_size += r;
    }
}

InputFormat LdfInputReader::detectContainer() {
    fillPushback(4);
    if (m_pushback_size < 4)
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

namespace {
    // The Ogg page checksum: CRC-32 with the 0x04C11DB7 polynomial, no
    // reflection, zero initial value, over the page with its CRC field zeroed
    uint32_t oggCrc(const uint8_t *p, size_t n) {
        uint32_t crc = 0;
        for (size_t i = 0; i < n; i++) {
            crc ^= (uint32_t)p[i] << 24;
            for (int b = 0; b < 8; b++)
                crc = crc & 0x80000000u ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
        }
        return crc;
    }

    uint8_t flacCrc8(const uint8_t *p, size_t n) {
        uint8_t crc = 0;
        for (size_t i = 0; i < n; i++) {
            crc ^= p[i];
            for (int b = 0; b < 8; b++)
                crc = crc & 0x80 ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
        return crc;
    }

    // Decodes a FLAC frame header at p and returns the stream position just
    // after the frame, or -1 when there is no valid header here.  The frame
    // number (fixed block size streams) or sample number (variable) is
    // UTF-8 coded; the header ends with a CRC-8 that validates the match.
    int64_t frameEndFromHeader(const uint8_t *p, size_t n, int fixed_blocksize) {
        if (n < 6 || p[0] != 0xFF || (p[1] & 0xFE) != 0xF8)
            return -1;
        const bool variable = p[1] & 1;
        const int bs_code = p[2] >> 4, sr_code = p[2] & 15;
        if (bs_code == 0 || sr_code == 15 || (p[3] & 1))
            return -1;
        size_t i = 4;
        int len = 0;
        for (uint8_t b = p[i]; b & 0x80; b <<= 1)
            len++;
        if (len == 1 || len > 7)
            return -1;
        uint64_t number = len == 0 ? p[i] : p[i] & (0x7F >> len);
        if (i + std::max(len, 1) >= n)
            return -1;
        for (int k = 1; k < len; k++) {
            if ((p[i + k] & 0xC0) != 0x80)
                return -1;
            number = (number << 6) | (p[i + k] & 0x3F);
        }
        i += std::max(len, 1);
        int blocksize;
        if (bs_code == 1) blocksize = 192;
        else if (bs_code <= 5) blocksize = 576 << (bs_code - 2);
        else if (bs_code == 6) { if (i + 1 >= n) return -1; blocksize = p[i] + 1; i += 1; }
        else if (bs_code == 7) { if (i + 2 >= n) return -1; blocksize = (p[i] << 8 | p[i + 1]) + 1; i += 2; }
        else blocksize = 256 << (bs_code - 8);
        if (sr_code == 12) i += 1;
        else if (sr_code == 13 || sr_code == 14) i += 2;
        if (i >= n || flacCrc8(p, i) != p[i])
            return -1;
        if (!variable && fixed_blocksize > 0 && blocksize > fixed_blocksize)
            return -1; // only the last frame may differ, and only by being shorter
        const int64_t start = variable ? (int64_t)number : (int64_t)number * fixed_blocksize;
        return start + blocksize;
    }
}

// STREAMINFO stores the sample count in 36 bits, which long captures exceed
// (2^36 samples is 28 minutes at 40 MHz).  Some encoders then write the
// count wrapped, and libFLAC, trusting it, refuses every seek beyond that
// value although the frames are there and decode fine.  With the field at
// zero (the "unknown" value, what libFLAC itself writes from a pipe) it
// seeks by the stream length instead, which works anywhere; the count is
// not needed from here, sampleCount() takes it from the file's tail.  So
// the field is zeroed in the bytes handed to libFLAC, before it parses them.
void LdfInputReader::patchStreamInfoTotal() {
    constexpr size_t c_plain_head = 4 + 4 + 34;           // fLaC, block header, STREAMINFO
    constexpr size_t c_ogg_head = 27 + 1 + 9 + 4 + 4 + 34; // page header, lacing, FLAC-in-Ogg packet
    constexpr size_t c_total_offset = 10 + 3;              // in STREAMINFO: low nibble of this byte and the next four
    uint8_t *streaminfo = nullptr;
    if (m_container == eFlac) {
        fillPushback(c_plain_head);
        if (m_pushback_size < c_plain_head || (m_pushback[4] & 0x7F) != 0)
            return; // not a STREAMINFO block first: leave the stream alone
        streaminfo = m_pushback + 8;
    } else {
        fillPushback(c_ogg_head);
        if (m_pushback_size < c_ogg_head || m_pushback[26] != 1 || m_pushback[27] != 51
            || m_pushback[28] != 0x7F || memcmp(m_pushback + 29, "FLAC", 4) != 0
            || memcmp(m_pushback + 37, "fLaC", 4) != 0)
            return;
        streaminfo = m_pushback + 28 + 17;
    }
    uint64_t total = (uint64_t)(streaminfo[c_total_offset] & 0x0F) << 32;
    for (int k = 1; k <= 4; k++)
        total = total | (uint64_t)streaminfo[c_total_offset + k] << (8 * (4 - k));
    if (total == 0)
        return;
    streaminfo[c_total_offset] &= 0xF0;
    memset(streaminfo + c_total_offset + 1, 0, 4);
    if (m_container == eFlacOgg) {
        memset(m_pushback + 22, 0, 4);
        const uint32_t crc = oggCrc(m_pushback, c_ogg_head);
        for (int k = 0; k < 4; k++)
            m_pushback[22 + k] = (uint8_t)(crc >> (8 * k));
    }
    if (m_log)
        m_log->debug(eInput, std::format("FLAC STREAMINFO sample count {} hidden from libFLAC so seeks are not "
                                         "limited by it (the field holds 36 bits)", total));
}

void LdfInputReader::initialize() {
    m_container = detectContainer();
    if (!isLive())
        patchStreamInfoTotal();
    auto status = m_container == eFlac ? init() : init_ogg();
    if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK)
        throw std::runtime_error(std::format("Error initializing decoder: {}", FLAC__StreamDecoderInitStatusString[status]));

    // Perform first processing so we can seek before readFloats is called
    FLAC__StreamDecoderState s;
    while (s = get_state(), s != FLAC__STREAM_DECODER_READ_FRAME && s != FLAC__STREAM_DECODER_SEARCH_FOR_FRAME_SYNC && s != FLAC__STREAM_DECODER_END_OF_STREAM)
        processSingleChecked();
}

int64_t LdfInputReader::sampleCount() {
    if (m_total_samples > 0)
        return m_total_samples;
    if (!m_sample_count_estimate)
        m_sample_count_estimate = m_container == eFlacOgg ? sampleCountFromOggGranule() : sampleCountFromLastFrame();
    return *m_sample_count_estimate;
}

// Plain FLAC without a usable STREAMINFO count (see patchStreamInfoTotal):
// the last frame header in the file says where its frame ends.
int64_t LdfInputReader::sampleCountFromLastFrame() {
    if (isLive())
        return -1;
    std::scoped_lock<std::mutex> lock(m_source_mutex);
    const int64_t size = m_source->size();
    if (size <= 0)
        return -1;
    const int64_t original = m_source->seek(0, SEEK_CUR);
    if (original < 0)
        return -1;
    constexpr int64_t tail = 256 * 1024; // several frames of RF at any compression
    const int64_t start = std::max<int64_t>(0, size - tail);
    std::vector<uint8_t> buf((size_t)(size - start));
    int64_t result = -1;
    if (m_source->seek(start, SEEK_SET) == start) {
        size_t got = 0;
        while (got < buf.size()) {
            const ssize_t n = m_source->read(buf.data() + got, buf.size() - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
        // The last header whose CRC-8 checks out.  A false match (compressed
        // data imitating a header) would have to pass the CRC and land after
        // the real last frame, so the frame end is also required to stay
        // ahead of the previous valid one by less than one tail's worth.
        int64_t last = -1;
        for (size_t i = 0; i + 16 <= got; i++) {
            const int64_t end = frameEndFromHeader(buf.data() + i, got - i, m_streaminfo_blocksize);
            if (end < 0)
                continue;
            if (last < 0 || (end > last && end - last <= tail * 8))
                last = end;
        }
        result = last;
    }
    m_source->seek(original, SEEK_SET);
    return result;
}

// ld-decode's .ldf files are FLAC in Ogg written through a pipe, so their
// STREAMINFO says nothing about the length.  The Ogg pages do: each carries
// the granule position, which for FLAC in Ogg is the number of samples
// encoded up to the end of the page, so the last page of the file gives
// the length.  Read
// the tail of the source directly, under the source lock, and put the
// position back for libFLAC, which keeps its own buffer and notices nothing.
int64_t LdfInputReader::sampleCountFromOggGranule() {
    if (isLive())
        return -1;
    std::scoped_lock<std::mutex> lock(m_source_mutex);
    const int64_t size = m_source->size();
    if (size <= 0)
        return -1;
    const int64_t original = m_source->seek(0, SEEK_CUR);
    if (original < 0)
        return -1;
    // Pages are 64 KiB at most; a few of them cover a truncated tail too
    constexpr int64_t tail = 256 * 1024;
    const int64_t start = std::max<int64_t>(0, size - tail);
    std::vector<uint8_t> buf((size_t)(size - start));
    int64_t result = -1;
    if (m_source->seek(start, SEEK_SET) == start) {
        size_t got = 0;
        while (got < buf.size()) {
            const ssize_t n = m_source->read(buf.data() + got, buf.size() - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
        // The last "OggS" capture pattern with a granule position that is
        // not the -1 of a page holding no packet end
        for (size_t i = got >= 27 ? got - 27 : 0; i-- > 0;) {
            if (buf[i] != 'O' || buf[i + 1] != 'g' || buf[i + 2] != 'g' || buf[i + 3] != 'S' || buf[i + 4] != 0)
                continue;
            uint64_t granule = 0;
            for (int b = 7; b >= 0; b--)
                granule = (granule << 8) | buf[i + 6 + b];
            if (granule != ~0ULL && granule != 0) {
                result = (int64_t)granule;
                break;
            }
        }
    }
    m_source->seek(original, SEEK_SET);
    return result;
}

bool LdfInputReader::seek(int64_t no_samples) {
    if (isLive())
        return true;
    std::scoped_lock<std::mutex> lock(m_source_mutex);
    throwIfFailed();
    // Clamp at the start of the stream (see InputReaderImpl::seek)
    no_samples = std::max(no_samples, -(int64_t)m_sample_position);
    bool ok = seek_absolute(m_sample_position + no_samples);
    throwIfFailed();
    if (ok) {
        m_sample_position += no_samples;
        return true;
    }
    // libFLAC refused the position (past the end, or beyond a STREAMINFO
    // count it trusts).  It is left in its seek-error state, so put it back
    // where it was and let the caller carry on from there.
    if (m_log)
        m_log->warn(eInput, std::format("libFLAC cannot seek to sample {}; staying at {}",
                                        m_sample_position + no_samples, m_sample_position));
    flush();
    ok = seek_absolute(m_sample_position);
    throwIfFailed();
    if (!ok)
        throw std::runtime_error("libflac: seek_absolute failed, and the previous position cannot be restored");
    return false;
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
        // Any width up to the 16 bits the sample store holds: captures come as 8, 10
        // (Domesday Duplicator) or 16 bits
        m_total_samples = (int64_t)metadata->data.stream_info.total_samples;
        m_streaminfo_blocksize = (int)metadata->data.stream_info.max_blocksize;
        const auto bits = metadata->data.stream_info.bits_per_sample;
        if (bits >= 4 && bits <= 16)
            m_bits_per_sample = (int)bits;
        else
            recordError(std::format("LDF files should have 4 to 16 bits per sample, not {}", bits));
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
