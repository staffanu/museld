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

uint64_t LdfInputReader::s_streaminfo_count_limit = (uint64_t)1 << 36;

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

    // The CRC-16 that ends every FLAC frame: polynomial 0x8005, zero initial value, over the
    // frame from its sync code to the byte before the CRC
    uint16_t flacCrc16(const uint8_t *p, size_t n) {
        uint16_t crc = 0;
        for (size_t i = 0; i < n; i++) {
            crc ^= (uint16_t)(p[i] << 8);
            for (int b = 0; b < 8; b++)
                crc = crc & 0x8000 ? (uint16_t)((crc << 1) ^ 0x8005) : (uint16_t)(crc << 1);
        }
        return crc;
    }

    // Decodes a FLAC frame header at p and returns the stream position just
    // after the frame, or -1 when there is no valid header here (and the
    // frame's first sample in *start_out, when asked for).  The frame
    // number (fixed block size streams) or sample number (variable) is
    // UTF-8 coded; the header ends with a CRC-8 that validates the match.
    int64_t frameEndFromHeader(const uint8_t *p, size_t n, int fixed_blocksize, int64_t *start_out = nullptr) {
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
        if (start_out)
            *start_out = start;
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
//
// A plain FLAC stream is the other way round: libFLAC seeks it by bisection
// between the count and the stream length, and without a count it starts
// from the stream's end, where its first probe must decode the last frame.
// That probe is placed a guessed frame length before the end, and when it
// lands inside the last frame (or the file ends inside a frame: flac still
// writing, or killed) no frame decodes there, and libFLAC either gives up
// or, told by eof_callback that it reached the end, walks the file from
// its lower bound one frame at a time (seconds on a 200 MB file).  So a
// plain FLAC stream without a usable count gets the exact count from the
// last frame header in its tail, written into the field for libFLAC (when
// it fits the 36 bits) -- its probes then interpolate towards the target
// and a target past the end is refused at once -- and a file holding only
// part of its last frame has the stream ended for libFLAC at that frame's
// header (m_stream_end: length_callback, read_callback, eof_callback), so
// neither the seeks nor the playback reaching it see the partial frame.
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
    const auto write_total = [&](uint64_t value) {
        streaminfo[c_total_offset] = (uint8_t)((streaminfo[c_total_offset] & 0xF0) | ((value >> 32) & 0x0F));
        for (int k = 1; k <= 4; k++)
            streaminfo[c_total_offset + k] = (uint8_t)(value >> (8 * (4 - k)));
    };
    if (m_container == eFlac) {
        const int fixed_blocksize = streaminfo[2] << 8 | streaminfo[3]; // the maximum block size
        const int bits_per_sample = (((streaminfo[12] & 0x01) << 4) | (streaminfo[13] >> 4)) + 1;
        const int64_t size = m_source->size();
        // The count is kept when it can be right: the file holds no more bytes than
        // that many samples could compress to.  FLAC does not expand RF (0.3-0.6 of
        // the raw size on the captures here), so a ratio above 1.1 means a wrapped count.
        if (total != 0 && size > 0 && (double)size * 8.0 <= 1.1 * (double)total * (double)bits_per_sample) {
            if (m_log)
                m_log->debug(eInput, std::format("FLAC STREAMINFO sample count {} kept: plausible for {} bytes at {} bits",
                                                 total, size, bits_per_sample));
            return;
        }
        const auto last = scanLastFrame(fixed_blocksize);
        if (last) {
            const int64_t count = last->complete ? last->end_sample : last->start_sample;
            m_sample_count_estimate = count;
            if (!last->complete) {
                m_stream_end = last->header_offset;
                if (m_log)
                    m_log->info(eInput, std::format("The file holds only part of its last FLAC frame (samples {} to {}; the "
                                                    "encoder was still writing, or was stopped), so the stream ends before it, "
                                                    "at byte {}", last->start_sample, last->end_sample, last->header_offset));
            }
            if ((uint64_t)count < s_streaminfo_count_limit) {
                write_total((uint64_t)count);
                if (m_log)
                    m_log->debug(eInput, std::format("FLAC STREAMINFO sample count {} written from the file's last frame "
                                                     "header for libFLAC to seek by (the field held {})", count, total));
                return;
            }
            if (m_log)
                m_log->debug(eInput, std::format("FLAC sample count {} from the file's last frame header does not fit "
                                                 "STREAMINFO's 36 bits: the reader seeks by its own search (the field held {})",
                                                 count, total));
        }
        if (total == 0)
            return;
        write_total(0);
        if (m_log)
            m_log->debug(eInput, std::format("FLAC STREAMINFO sample count {} hidden from libFLAC so seeks are not "
                                             "limited by it (the field holds 36 bits)", total));
        return;
    }
    if (total == 0)
        return;
    write_total(0);
    memset(m_pushback + 22, 0, 4);
    const uint32_t crc = oggCrc(m_pushback, c_ogg_head);
    for (int k = 0; k < 4; k++)
        m_pushback[22 + k] = (uint8_t)(crc >> (8 * k));
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
    // Where the frames start: nothing of them is consumed yet (seekBySearch's lower bound)
    FLAC__uint64 first_frame = 0;
    if (m_container == eFlac && get_decode_position(&first_frame))
        m_first_frame_offset = (int64_t)first_frame;
}

int64_t LdfInputReader::sampleCount() {
    if (m_total_samples > 0)
        return m_total_samples;
    if (!m_sample_count_estimate)
        m_sample_count_estimate = m_container == eFlacOgg ? sampleCountFromOggGranule() : sampleCountFromLastFrame();
    return *m_sample_count_estimate;
}

// Plain FLAC without a usable STREAMINFO count (see patchStreamInfoTotal):
// the last frame header in the file says where its frame ends -- or, when
// the file holds only part of that frame, where it begins, since the stream
// is ended there for libFLAC.
int64_t LdfInputReader::sampleCountFromLastFrame() {
    const auto last = scanLastFrame(m_streaminfo_blocksize);
    if (!last)
        return -1;
    return last->complete ? last->end_sample : last->start_sample;
}

std::optional<LdfInputReader::LastFrame> LdfInputReader::scanLastFrame(int fixed_blocksize) {
    if (isLive())
        return std::nullopt;
    std::scoped_lock<std::mutex> lock(m_source_mutex);
    const int64_t size = m_source->size();
    if (size <= 0)
        return std::nullopt;
    const int64_t original = m_source->seek(0, SEEK_CUR);
    if (original < 0)
        return std::nullopt;
    constexpr int64_t tail = 256 * 1024; // several frames of RF at any compression
    const int64_t start = std::max<int64_t>(0, size - tail);
    std::vector<uint8_t> buf((size_t)(size - start));
    std::optional<LastFrame> result;
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
        int64_t last = -1, last_start = -1;
        size_t last_at = 0;
        for (size_t i = 0; i + 16 <= got; i++) {
            int64_t frame_start = 0;
            const int64_t end = frameEndFromHeader(buf.data() + i, got - i, fixed_blocksize, &frame_start);
            if (end < 0)
                continue;
            if (last < 0 || (end > last && end - last <= tail * 8)) {
                last = end;
                last_start = frame_start;
                last_at = i;
            }
        }
        if (last >= 0) {
            // The frame is whole when the file ends with its CRC-16.  An encoder that was
            // still writing, or was stopped, leaves part of a frame instead.
            const bool whole_file_read = got == buf.size();
            const size_t frame_bytes = got - last_at;
            const bool complete = whole_file_read && frame_bytes > 2
                && flacCrc16(buf.data() + last_at, frame_bytes - 2)
                    == (uint16_t)(buf[got - 2] << 8 | buf[got - 1]);
            result = LastFrame{start + (int64_t)last_at, last_start, last, complete};
        }
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
    const int64_t target = (int64_t)m_sample_position + no_samples;
    // A plain FLAC stream libFLAC has no count for (see patchStreamInfoTotal) is
    // seeked by the reader's own search, with the count it read from the tail
    const bool own_search = m_container == eFlac && m_total_samples == 0 && m_first_frame_offset >= 0
        && m_sample_count_estimate && *m_sample_count_estimate > 0;
    if (own_search && target >= *m_sample_count_estimate) {
        if (m_log)
            m_log->warn(eInput, std::format("Cannot seek to sample {}: the file holds {}; staying at {}",
                                            target, *m_sample_count_estimate, m_sample_position));
        return false; // nothing moved
    }
    // Where libFLAC reads next (the byte after the last frame it decoded), to put it back
    // there by offset if the seek fails: a second bisection would fail where the first one
    // did.  Not known for Ogg, where libFLAC reads through its own page layer.  The samples
    // not yet taken from the last decoded frame are kept too: the search decodes over them.
    FLAC__uint64 decode_position = 0;
    const bool have_position = m_container == eFlac && get_decode_position(&decode_position);
    const std::vector<uint16_t> kept_samples(m_decoded_samples, m_decoded_samples + m_flac_used_size);
    const uint32_t kept_read_count = m_flac_block_read_count;
    bool ok;
    if (own_search) {
        m_probing = true;
        ok = seekBySearch(target, *m_sample_count_estimate);
        m_probing = false;
    } else {
        ok = seek_absolute((FLAC__uint64)target);
    }
    throwIfFailed();
    if (ok) {
        m_sample_position = (uint64_t)target;
        return true;
    }
    // libFLAC refused the position (past the end, or beyond a STREAMINFO count it trusts),
    // or the search found no frame holding it.  The decoder is left where the attempt ended,
    // so put it back where it was and let the caller carry on from there.
    if (m_log)
        m_log->warn(eInput, std::format("{} cannot seek to sample {}; staying at {}",
                                        own_search ? "The frame search" : "libFLAC", target, m_sample_position));
    flush(); // the decoder looks for a frame sync where the source is positioned next
    if (have_position && seek_callback(decode_position) == FLAC__STREAM_DECODER_SEEK_STATUS_OK) {
        if (own_search) {
            memcpy(m_decoded_samples, kept_samples.data(), kept_samples.size() * sizeof(uint16_t));
            m_flac_used_size = (uint32_t)kept_samples.size();
            m_flac_block_read_count = kept_read_count;
        }
        return false;
    }
    ok = seek_absolute(m_sample_position);
    throwIfFailed();
    if (!ok)
        throw std::runtime_error("libflac: seek_absolute failed, and the previous position cannot be restored");
    return false;
}

// Positions libFLAC at `offset` and decodes the first frame after it, which write_callback
// records in m_last_frame_sample.  False when nothing decodes there: the stream's data ends
// before the offset, or a read failed (recorded for the caller to raise).
bool LdfInputReader::decodeFrameAt(int64_t offset) {
    if (seek_callback((FLAC__uint64)offset) != FLAC__STREAM_DECODER_SEEK_STATUS_OK || !flush())
        return false;
    m_last_frame_sample = -1;
    // A false frame sync in the compressed data costs a step that decodes nothing (the
    // error is reported to error_callback, which ignores it while probing) and libFLAC
    // resumes its search after it
    for (int step = 0; step < 8 && m_last_frame_sample < 0; step++) {
        if (!process_single() || !m_failure.empty() || get_state() == FLAC__STREAM_DECODER_END_OF_STREAM)
            return false;
    }
    return m_last_frame_sample >= 0;
}

// libFLAC seeks a plain FLAC stream of unknown length by bisection from its end, where its
// first probe must decode the last frame -- which fails whenever the probe, placed a guessed
// frame length before the end, lands inside that frame.  A stream of more than 2^36 samples
// (a disc side at 62.5 MHz) cannot be given its count, so its target is found here instead:
// an interpolation search between the first frame and the stream's end (byte and sample
// known at both, the end's from the tail scan), each probe decoding the frame after its
// offset, narrowed to a stretch of a few frames that is then decoded forward to the frame
// holding the target.  That frame is left in m_decoded_samples with the read position at the
// target, as libFLAC's own seek leaves it.  The caller checked the target against the count.
bool LdfInputReader::seekBySearch(int64_t target, int64_t count) {
    const int64_t end = m_stream_end >= 0 ? m_stream_end : m_source->size();
    if (end <= m_first_frame_offset || target < 0)
        return false;
    int64_t lo_bytes = m_first_frame_offset, lo_sample = 0, hi_bytes = end, hi_sample = count;
    constexpr int64_t c_forward_bytes = 64 * 1024; // decoded frame by frame from here: tens of frames
    for (int probe = 0; probe < 40 && hi_bytes - lo_bytes > c_forward_bytes; probe++) {
        // Aim by the sample density between the bounds; the bounds move onto the probe's own
        // offset, so every probe narrows the stretch
        const double fraction = hi_sample > lo_sample ? (double)(target - lo_sample) / (double)(hi_sample - lo_sample) : 0.5;
        const int64_t pos = std::clamp(lo_bytes + (int64_t)(fraction * (double)(hi_bytes - lo_bytes)),
                                       lo_bytes + 1, hi_bytes - 1);
        if (!decodeFrameAt(pos)) {
            if (!m_failure.empty())
                return false;
            hi_bytes = pos; // no frame begins after pos: the data ends before it
            continue;
        }
        const int64_t frame_sample = m_last_frame_sample;
        if (frame_sample <= target && target < frame_sample + m_last_frame_blocksize) {
            m_flac_block_read_count = (uint32_t)(target - frame_sample);
            return true;
        }
        // The first frame after pos starts at frame_sample, so the frame holding the target
        // starts before pos when it is earlier, after pos when it is later
        if (frame_sample > target) {
            hi_bytes = pos;
            hi_sample = frame_sample;
        } else {
            lo_bytes = pos;
            lo_sample = frame_sample;
        }
    }
    // Forward from the lower bound to the frame holding the target
    if (!decodeFrameAt(lo_bytes))
        return false;
    for (int frames = 0; frames < 4096; frames++) {
        if (m_last_frame_sample <= target && target < m_last_frame_sample + m_last_frame_blocksize) {
            m_flac_block_read_count = (uint32_t)(target - m_last_frame_sample);
            return true;
        }
        if (m_last_frame_sample > target)
            return false; // the target lies in no frame: a hole in the stream
        m_last_frame_sample = -1;
        if (!process_single() || !m_failure.empty() || get_state() == FLAC__STREAM_DECODER_END_OF_STREAM)
            return false;
        if (m_last_frame_sample < 0)
            frames--; // a step that decoded nothing (a bad frame skipped)
    }
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
    size_t wanted = *bytes;
    if (m_stream_end >= 0) {
        // The stream ends before the file does (see patchStreamInfoTotal)
        const int64_t position = m_source->seek(0, SEEK_CUR);
        if (position >= 0)
            wanted = (size_t)std::clamp<int64_t>(m_stream_end - position, 0, (int64_t)wanted);
        if (wanted == 0) {
            *bytes = 0;
            return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
        }
    }
    // libFLAC has no "try again later": a live source that has nothing yet is
    // waited for here, as the raw readers do in readFully.
    ssize_t r;
    while ((r = m_source->read(buffer, wanted)) == -1 && errno == EAGAIN)
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
    // libFLAC turns a frame number into the sample number when it knows the block size
    m_last_frame_sample = frame->header.number_type == FLAC__FRAME_NUMBER_TYPE_FRAME_NUMBER
        ? (int64_t)frame->header.number.frame_number * frame->header.blocksize
        : (int64_t)frame->header.number.sample_number;
    m_last_frame_blocksize = (int)frame->header.blocksize;

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
    if (m_probing)
        return; // a probe landing in the middle of a frame finds false syncs; libFLAC moves on
    recordError(std::format("libFLAC++ error_callback: {}", FLAC__StreamDecoderErrorStatusString[status]));
}

int64_t LdfInputReader::sourcePosition() {
    const int64_t position = m_source->seek(0, SEEK_CUR);
    if (position == -1)
        return -1;
    return position - (int64_t)(m_pushback_size - m_pushback_read);
}

FLAC__StreamDecoderTellStatus LdfInputReader::tell_callback(FLAC__uint64 *absolute_byte_offset) {
    const int64_t position = sourcePosition();
    if (position == -1) {
        if (errno == ESPIPE)
            return FLAC__STREAM_DECODER_TELL_STATUS_UNSUPPORTED;
        return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
    }
    *absolute_byte_offset = (FLAC__uint64)position;
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

FLAC__StreamDecoderSeekStatus LdfInputReader::seek_callback(FLAC__uint64 absolute_byte_offset) {
    // The stream head is served from m_pushback, whose STREAMINFO may be patched
    // (patchStreamInfoTotal), so a seek into it reads from there, not the source
    const int64_t source_offset = std::max<int64_t>((int64_t)absolute_byte_offset, (int64_t)m_pushback_size);
    int64_t r = m_source->seek(source_offset, SEEK_SET);
    if (r == -1) {
        if (errno == ESPIPE)
            return FLAC__STREAM_DECODER_SEEK_STATUS_UNSUPPORTED;
        return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    }
    m_pushback_read = (size_t)std::min<int64_t>((int64_t)absolute_byte_offset, (int64_t)m_pushback_size);
    return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

FLAC__StreamDecoderLengthStatus LdfInputReader::length_callback(FLAC__uint64 *stream_length) {
    const int64_t size = m_source->size();
    if (size < 0)
        return FLAC__STREAM_DECODER_LENGTH_STATUS_UNSUPPORTED;
    *stream_length = (FLAC__uint64)(m_stream_end >= 0 ? std::min(m_stream_end, size) : size);
    return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

// Only where the stream is ended short of the file (patchStreamInfoTotal) is the end known
// here; otherwise libFLAC learns of it from a read that returns nothing, as before, which is
// also all a live source can tell.
bool LdfInputReader::eof_callback() {
    if (m_stream_end < 0)
        return false;
    const int64_t position = sourcePosition();
    return position >= 0 && position >= m_stream_end;
}
