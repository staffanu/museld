// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <stdexcept>
#include <FLAC++/decoder.h>
#include <pthread.h>
#include "ParallelOggFlacReader.h"
#include "logging/Logger.h"
#include "util/ThreadRole.h"

namespace {

constexpr size_t c_run_bytes = 2u << 20; // a run is at least this many bytes of pages
constexpr size_t c_page_header = 27;
constexpr size_t c_max_page = 255 * 255 + c_page_header + 255; // the Ogg maximum

// The Ogg page CRC: CRC-32 with the 0x04C11DB7 polynomial, no reflection, zero
// initial value, over the page with its CRC field zeroed
struct OggCrcTable {
    uint32_t t[256];
    OggCrcTable() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t crc = i << 24;
            for (int b = 0; b < 8; b++)
                crc = crc & 0x80000000u ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
            t[i] = crc;
        }
    }
};
const OggCrcTable &crcTable() {
    static const OggCrcTable table;
    return table;
}
uint32_t oggCrc(const uint8_t *p, size_t n) {
    const auto &t = crcTable().t;
    uint32_t crc = 0;
    for (size_t i = 0; i < n; i++)
        crc = (crc << 8) ^ t[((crc >> 24) ^ p[i]) & 0xFF];
    return crc;
}

struct PageInfo {
    size_t length = 0;      // the whole page
    int64_t granule = -1;   // samples up to the page's end, -1 when no packet ends here
    bool fresh = false;     // the first packet begins on this page
    uint32_t serial = 0;
    uint32_t sequence = 0;
};

// Parses the page header at p.  Returns false when there is no page here or
// the header is not complete within `avail` bytes.
bool parsePage(const uint8_t *p, size_t avail, PageInfo &info) {
    if (avail < c_page_header || memcmp(p, "OggS", 4) != 0 || p[4] != 0)
        return false;
    const size_t nseg = p[26];
    if (avail < c_page_header + nseg)
        return false;
    size_t length = c_page_header + nseg;
    for (size_t i = 0; i < nseg; i++)
        length += p[c_page_header + i];
    info.length = length;
    uint64_t granule = 0;
    for (int b = 7; b >= 0; b--)
        granule = (granule << 8) | p[6 + b];
    info.granule = granule == ~0ULL ? -1 : (int64_t)granule;
    info.fresh = (p[5] & 1) == 0;
    info.serial = (uint32_t)p[14] | (uint32_t)p[15] << 8 | (uint32_t)p[16] << 16 | (uint32_t)p[17] << 24;
    info.sequence = (uint32_t)p[18] | (uint32_t)p[19] << 8 | (uint32_t)p[20] << 16 | (uint32_t)p[21] << 24;
    return true;
}

bool pageCrcOk(const uint8_t *p, size_t length) {
    uint8_t copy[c_page_header];
    memcpy(copy, p, c_page_header);
    memset(copy + 22, 0, 4);
    uint32_t crc = 0;
    const auto &t = crcTable().t;
    for (size_t i = 0; i < c_page_header; i++)
        crc = (crc << 8) ^ t[((crc >> 24) ^ copy[i]) & 0xFF];
    for (size_t i = c_page_header; i < length; i++)
        crc = (crc << 8) ^ t[((crc >> 24) ^ p[i]) & 0xFF];
    const uint32_t stored = (uint32_t)p[22] | (uint32_t)p[23] << 8 | (uint32_t)p[24] << 16 | (uint32_t)p[25] << 24;
    return crc == stored;
}

// Gives the page a new sequence number and a checksum to match
void renumberPage(uint8_t *p, size_t length, uint32_t sequence) {
    for (int k = 0; k < 4; k++)
        p[18 + k] = (uint8_t)(sequence >> (8 * k));
    memset(p + 22, 0, 4);
    const uint32_t crc = oggCrc(p, length);
    for (int k = 0; k < 4; k++)
        p[22 + k] = (uint8_t)(crc >> (8 * k));
}

} // namespace

// One libFLAC decoder, fed the header pages and then one run from memory
class ParallelOggFlacReader::RunDecoder : private FLAC::Decoder::Stream {
public:
    explicit RunDecoder(const std::vector<uint8_t> &headers) : m_headers(headers) {
        const auto status = init_ogg();
        if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK)
            throw std::runtime_error(std::format("Error initializing decoder: {}", FLAC__StreamDecoderInitStatusString[status]));
    }

    // Decodes `run` into `out`; false with the reason in out.error on failure
    bool decodeRun(const std::vector<uint8_t> &run, Decoded &out) {
        m_run = &run;
        m_out = &out;
        m_read_pos = 0;
        m_failure.clear();
        out.first_sample = -1;
        out.samples.clear();
        if (!reset()) {
            out.error = "libFLAC++ reset failed";
            return out.ok = false;
        }
        for (;;) {
            const bool ok = process_single();
            if (!m_failure.empty()) {
                out.error = m_failure;
                return out.ok = false;
            }
            const auto state = get_state();
            if (state == FLAC__STREAM_DECODER_END_OF_STREAM)
                break;
            if (!ok || state == FLAC__STREAM_DECODER_ABORTED || state == FLAC__STREAM_DECODER_MEMORY_ALLOCATION_ERROR) {
                out.error = std::format("libFLAC++ process_single: {}", FLAC__StreamDecoderStateString[state]);
                return out.ok = false;
            }
        }
        return out.ok = true;
    }

    int bitsPerSample() const { return m_bits_per_sample; }
    int fixedBlocksize() const { return m_fixed_blocksize; }

private:
    FLAC__StreamDecoderReadStatus read_callback(FLAC__byte buffer[], size_t *bytes) override {
        size_t n = 0;
        if (m_read_pos < m_headers.size()) {
            n = std::min(*bytes, m_headers.size() - m_read_pos);
            memcpy(buffer, m_headers.data() + m_read_pos, n);
        } else {
            const size_t at = m_read_pos - m_headers.size();
            if (at < m_run->size()) {
                n = std::min(*bytes, m_run->size() - at);
                memcpy(buffer, m_run->data() + at, n);
            }
        }
        m_read_pos += n;
        *bytes = n;
        return n == 0 ? FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM : FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }

    FLAC__StreamDecoderWriteStatus write_callback(const ::FLAC__Frame *frame, const FLAC__int32 *const buffer[]) override {
        const int64_t number = frame->header.number_type == FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER
                ? (int64_t)frame->header.number.sample_number
                : (int64_t)frame->header.number.frame_number * m_fixed_blocksize;
        if (m_out->first_sample < 0)
            m_out->first_sample = number;
        else if (number != m_out->first_sample + (int64_t)m_out->samples.size()) {
            m_failure = std::format("FLAC frame at sample {} where {} was expected",
                                    number, m_out->first_sample + (int64_t)m_out->samples.size());
            return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }
        const size_t at = m_out->samples.size();
        m_out->samples.resize(at + frame->header.blocksize);
        for (unsigned i = 0; i < frame->header.blocksize; i++)
            m_out->samples[at + i] = (int16_t)buffer[0][i];
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }

    void metadata_callback(const ::FLAC__StreamMetadata *metadata) override {
        if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
            if (metadata->data.stream_info.channels != 1)
                m_failure = "LDF files should have only one channel";
            const auto bits = metadata->data.stream_info.bits_per_sample;
            if (bits >= 4 && bits <= 16)
                m_bits_per_sample = (int)bits;
            else
                m_failure = std::format("LDF files should have 4 to 16 bits per sample, not {}", bits);
            m_fixed_blocksize = (int)metadata->data.stream_info.max_blocksize;
        }
    }

    void error_callback(::FLAC__StreamDecoderErrorStatus status) override {
        if (m_failure.empty())
            m_failure = std::format("libFLAC++ error_callback: {}", FLAC__StreamDecoderErrorStatusString[status]);
    }

    const std::vector<uint8_t> &m_headers;
    const std::vector<uint8_t> *m_run = nullptr;
    Decoded *m_out = nullptr;
    size_t m_read_pos = 0;
    int m_bits_per_sample = 0;
    int m_fixed_blocksize = 0;
    std::string m_failure;
};

ParallelOggFlacReader::ParallelOggFlacReader(std::unique_ptr<ByteSource> source, uint32_t block_size, int threads, Logger *log)
    : InputReader(std::move(source), block_size), m_log(log), m_threads(std::max(1, threads)) {
    if (m_source->isLive())
        throw std::runtime_error("ParallelOggFlacReader: the source must be seekable");
}

ParallelOggFlacReader::~ParallelOggFlacReader() {
    {
        std::scoped_lock lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    for (auto &t : m_decoders)
        if (t.joinable())
            t.join();
    if (m_scanner.joinable())
        m_scanner.join();
}

// Fills `buf` with `wanted` bytes from `offset` (fewer at the end of the stream).
// Caller holds the source: the scanner is parked or not started.
bool ParallelOggFlacReader::readPagesFrom(int64_t offset, std::vector<uint8_t> &buf, size_t wanted) {
    buf.resize(wanted);
    if (m_source->seek(offset, SEEK_SET) != offset)
        return false;
    size_t got = 0;
    while (got < wanted) {
        const ssize_t n = m_source->read(buf.data() + got, wanted - got);
        if (n == -1 && errno == EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (n == -1)
            throw std::runtime_error(std::format("Error reading input: {}", strerror(errno)));
        if (n == 0)
            break;
        got += (size_t)n;
    }
    buf.resize(got);
    return true;
}

// The byte offset of the first page that starts at or after `offset`, verified by
// its checksum, or -1 when there is none before `limit`
int64_t ParallelOggFlacReader::pageStartAfter(int64_t offset, int64_t limit) {
    std::vector<uint8_t> buf;
    while (offset < limit) {
        const size_t wanted = (size_t)std::min<int64_t>(limit - offset + (int64_t)c_max_page, 512 * 1024);
        if (!readPagesFrom(offset, buf, wanted) || buf.size() < c_page_header)
            return -1;
        for (size_t i = 0; i + c_page_header <= buf.size(); i++) {
            PageInfo info;
            if (!parsePage(buf.data() + i, buf.size() - i, info))
                continue;
            if (i + info.length <= buf.size() && pageCrcOk(buf.data() + i, info.length))
                return offset + (int64_t)i;
            if (i + info.length > buf.size() && offset + (int64_t)i + (int64_t)info.length <= limit + (int64_t)c_max_page) {
                // the page runs past the window: read it whole and check again
                std::vector<uint8_t> page;
                if (readPagesFrom(offset + (int64_t)i, page, info.length) && page.size() == info.length && pageCrcOk(page.data(), info.length))
                    return offset + (int64_t)i;
            }
        }
        offset += (int64_t)buf.size() - (int64_t)c_page_header; // overlap, a header may straddle
        if (buf.size() < wanted)
            return -1; // the end of the stream
    }
    return -1;
}

void ParallelOggFlacReader::initialize() {
    // The header pages: those at the start with a granule position of zero (the
    // identification packet with STREAMINFO, then the other metadata); the first
    // audio page follows
    std::vector<uint8_t> head;
    if (!readPagesFrom(0, head, 256 * 1024) || head.size() < c_page_header || memcmp(head.data(), "OggS", 4) != 0)
        throw std::runtime_error("ParallelOggFlacReader: the input is not FLAC in Ogg");
    size_t at = 0;
    for (;;) {
        PageInfo info;
        if (!parsePage(head.data() + at, head.size() - at, info) || at + info.length > head.size())
            throw std::runtime_error("ParallelOggFlacReader: no audio page within the first 256 KiB");
        if (at == 0)
            m_serial = info.serial;
        if (info.granule != 0)
            break; // the first audio page (a granule, or -1 under a packet that continues)
        at += info.length;
        m_header_pages++;
        if (m_header_pages > 64)
            throw std::runtime_error("ParallelOggFlacReader: too many header pages");
    }
    if (m_header_pages == 0)
        throw std::runtime_error("ParallelOggFlacReader: the stream has no header pages");
    m_headers.assign(head.begin(), head.begin() + (std::ptrdiff_t)at);
    m_data_start = (int64_t)at;

    // STREAMINFO through a decoder of the headers alone: the width, the block size
    {
        RunDecoder probe(m_headers);
        Decoded none;
        std::vector<uint8_t> empty;
        probe.decodeRun(empty, none);
        if (probe.bitsPerSample() == 0)
            throw std::runtime_error(std::format("ParallelOggFlacReader: cannot read the stream's STREAMINFO{}",
                                                 none.error.empty() ? "" : ": " + none.error));
        m_bits_per_sample = probe.bitsPerSample();
        m_fixed_blocksize = probe.fixedBlocksize();
    }

    // The length, from the last page's granule position (see LdfInputReader)
    const int64_t size = m_source->size();
    if (size > 0) {
        std::vector<uint8_t> tail;
        const int64_t start = std::max<int64_t>(0, size - 256 * 1024);
        if (readPagesFrom(start, tail, (size_t)(size - start))) {
            for (size_t i = tail.size() >= c_page_header ? tail.size() - c_page_header : 0; i-- > 0;) {
                PageInfo info;
                if (parsePage(tail.data() + i, tail.size() - i, info) && info.granule > 0) {
                    m_sample_count = info.granule;
                    break;
                }
            }
        }
    }
    if (m_log)
        m_log->info(eInput, std::format("FLAC in Ogg decoded on {} threads: {} header pages, {} bits per sample, {} samples",
                                        m_threads, m_header_pages, m_bits_per_sample,
                                        m_sample_count >= 0 ? std::to_string(m_sample_count) : "unknown count"));

    m_scan_offset = m_data_start;
    for (int i = 0; i < m_threads; i++)
        m_decoders.emplace_back([this, i] { decode(i); });
    m_scanner = std::thread([this] { scan(); });
}

// The scanner: reads pages in order from m_scan_offset and cuts them into runs
void ParallelOggFlacReader::scan() {
    setThreadRole(ThreadRole::eInput, "flac-scan");
    std::vector<uint8_t> buf;   // bytes read ahead from the source
    size_t buf_at = 0;          // the next unparsed byte
    int64_t buf_offset = 0;     // the stream offset of buf[0]
    bool positioned = false;    // the source is at buf_offset + buf.size()
    const size_t max_runs = (size_t)m_threads * 2 + 2;
    std::unique_lock lock(m_mutex);
    while (!m_stop) {
        if (m_scanner_park_request) {
            m_scanner_parked = true;
            m_cv.notify_all();
            m_cv.wait(lock, [this] { return m_stop || !m_scanner_park_request; });
            m_scanner_parked = false;
            positioned = false; // seek() moved the source and m_scan_offset
            buf.clear();
            buf_at = 0;
            continue;
        }
        if (m_scan_eof || m_runs.size() >= max_runs || !m_failure.empty()) {
            m_cv.wait(lock);
            continue;
        }
        const uint64_t generation = m_generation;
        const int64_t index = m_scan_next_index;
        if (!positioned) {
            buf_offset = m_scan_offset;
            buf.clear();
            buf_at = 0;
        }
        lock.unlock();
        // Build one run without the lock: read until the run is big enough and the
        // next page begins a fresh packet
        Run run{generation, index, {}};
        uint32_t run_pages = 0; // the pages follow the header pages in sequence
        bool eof = false;
        std::string failure;
        try {
            if (!positioned) {
                if (m_source->seek(buf_offset, SEEK_SET) != buf_offset)
                    throw std::runtime_error("ParallelOggFlacReader: seek failed");
                positioned = true;
            }
            for (;;) {
                // Make sure a whole page header, then the whole page, is in the buffer
                PageInfo info;
                bool have_page = false;
                for (;;) {
                    if (buf_at + c_page_header <= buf.size() && parsePage(buf.data() + buf_at, buf.size() - buf_at, info)
                        && buf_at + info.length <= buf.size()) {
                        have_page = true;
                        break;
                    }
                    if (buf_at + c_page_header <= buf.size() && memcmp(buf.data() + buf_at, "OggS", 4) != 0) {
                        // Not a page boundary: resynchronise on the next capture pattern
                        size_t i = buf_at + 1;
                        while (i + 4 <= buf.size() && memcmp(buf.data() + i, "OggS", 4) != 0)
                            i++;
                        if (m_log)
                            m_log->warn(eInput, std::format("FLAC in Ogg: no page at byte {}, skipping {} bytes",
                                                            buf_offset + (int64_t)buf_at, i - buf_at));
                        buf_at = i;
                        continue;
                    }
                    // Need more bytes: compact and read
                    if (buf_at > 0) {
                        buf.erase(buf.begin(), buf.begin() + (std::ptrdiff_t)buf_at);
                        buf_offset += (int64_t)buf_at;
                        buf_at = 0;
                    }
                    const size_t old = buf.size();
                    buf.resize(old + (1u << 20));
                    ssize_t n;
                    while ((n = m_source->read(buf.data() + old, 1u << 20)) == -1 && errno == EAGAIN)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    if (n == -1)
                        throw std::runtime_error(std::format("Error reading input: {}", strerror(errno)));
                    buf.resize(old + (size_t)n);
                    if (n == 0) {
                        eof = true;
                        break;
                    }
                }
                if (!have_page)
                    break; // the end: a partial page is dropped
                if (info.fresh && run.bytes.size() >= c_run_bytes)
                    break; // this page starts the next run
                const size_t at = run.bytes.size();
                run.bytes.insert(run.bytes.end(), buf.begin() + (std::ptrdiff_t)buf_at, buf.begin() + (std::ptrdiff_t)(buf_at + info.length));
                renumberPage(run.bytes.data() + at, info.length, m_header_pages + run_pages++);
                buf_at += info.length;
            }
        } catch (const std::exception &e) {
            failure = e.what();
        }
        lock.lock();
        if (!failure.empty()) {
            if (m_failure.empty())
                m_failure = failure;
            m_cv.notify_all();
            continue;
        }
        if (generation != m_generation)
            continue; // a seek happened: this run is for the old position
        if (!run.bytes.empty()) {
            m_runs.push_back(std::move(run));
            m_scan_next_index++;
        }
        m_scan_offset = buf_offset + (int64_t)buf_at;
        if (eof) {
            m_scan_eof = true;
            m_last_index = m_scan_next_index - 1;
        }
        m_cv.notify_all();
    }
}

void ParallelOggFlacReader::decode(int which) {
    setThreadRole(ThreadRole::eInput, "flac-decode");
    (void)which;
    std::unique_ptr<RunDecoder> decoder;
    std::unique_lock lock(m_mutex);
    while (!m_stop) {
        if (m_runs.empty() || !m_failure.empty()) {
            m_cv.wait(lock);
            continue;
        }
        Run run = std::move(m_runs.front());
        m_runs.pop_front();
        m_decoding++;
        m_cv.notify_all(); // the scanner may go on
        lock.unlock();
        Decoded out;
        std::string failure;
        try {
            if (!decoder)
                decoder = std::make_unique<RunDecoder>(m_headers);
            if (!decoder->decodeRun(run.bytes, out))
                failure = out.error;
        } catch (const std::exception &e) {
            failure = e.what();
        }
        lock.lock();
        m_decoding--;
        if (run.generation == m_generation) {
            if (!failure.empty() && m_failure.empty())
                m_failure = std::format("FLAC run {}: {}", run.index, failure);
            m_decoded.emplace(run.index, std::move(out));
        }
        m_cv.notify_all();
    }
}

void ParallelOggFlacReader::parkScanner(std::unique_lock<std::mutex> &lock) {
    m_scanner_park_request = true;
    m_cv.notify_all();
    m_cv.wait(lock, [this] { return m_scanner_parked || m_stop; });
}

void ParallelOggFlacReader::unparkScanner() {
    m_scanner_park_request = false;
    m_cv.notify_all();
}

// The byte offset of the page to start decoding at for `target_sample`: the last
// fresh page at or before the page where the target's frame ends.  Bisection on
// the granule positions, then a linear walk over a few pages.  Caller holds the
// source (the scanner is parked).
int64_t ParallelOggFlacReader::findRunStartFor(int64_t target_sample) {
    const int64_t size = m_source->size();
    if (size <= 0)
        return -1;
    int64_t lo = m_data_start, hi = size;
    // Invariant: the page at lo (the first page at or after lo) ends at or before
    // the target, or lo is the data start; pages at hi and beyond end after it
    while (hi - lo > 1 << 20) {
        const int64_t mid = lo + (hi - lo) / 2;
        int64_t p = pageStartAfter(mid, hi);
        PageInfo info;
        std::vector<uint8_t> buf;
        // Skip pages without a granule (a packet continues past them)
        while (p >= 0 && p < hi) {
            if (!readPagesFrom(p, buf, c_page_header + 255) || !parsePage(buf.data(), buf.size(), info))
                return -1;
            if (info.granule >= 0)
                break;
            p = pageStartAfter(p + (int64_t)info.length, hi);
        }
        if (p < 0 || p >= hi) {
            hi = mid; // nothing decisive between mid and hi: it is below
            continue;
        }
        if (info.granule <= target_sample)
            lo = p;
        else
            hi = p;
    }
    // Walk from lo: the last page whose granule is at or below the target, and
    // the fresh page that begins the frame after it
    int64_t p = pageStartAfter(lo, size), start = -1, last_fresh = -1;
    std::vector<uint8_t> buf;
    while (p >= 0 && p < size) {
        PageInfo info;
        if (!readPagesFrom(p, buf, c_page_header + 255) || !parsePage(buf.data(), buf.size(), info))
            break;
        if (info.fresh)
            last_fresh = p;
        if (info.granule > target_sample) {
            // The target's frame ends on this page: it began on the last fresh page
            start = last_fresh >= 0 ? last_fresh : p;
            break;
        }
        p += (int64_t)info.length;
    }
    if (start < 0)
        start = last_fresh >= 0 ? last_fresh : lo; // the target is in the last page or beyond
    return start;
}

bool ParallelOggFlacReader::seek(int64_t no_samples) {
    std::unique_lock lock(m_mutex);
    if (!m_failure.empty())
        throw std::runtime_error(m_failure);
    int64_t target = std::max<int64_t>(0, m_sample_position + no_samples);
    if (m_sample_count > 0 && target > m_sample_count) {
        if (m_log)
            m_log->warn(eInput, std::format("FLAC: cannot seek to sample {} of {}; staying at {}",
                                            target, m_sample_count, m_sample_position));
        return false;
    }
    parkScanner(lock);
    int64_t start;
    try {
        start = findRunStartFor(target);
    } catch (...) {
        unparkScanner();
        throw;
    }
    if (start < 0) {
        unparkScanner();
        if (m_log)
            m_log->warn(eInput, std::format("FLAC: no page found for sample {}; staying at {}", target, m_sample_position));
        return false;
    }
    m_generation++;
    m_runs.clear();
    m_decoded.clear();
    m_scan_offset = start;
    m_scan_next_index = 0;
    m_scan_eof = false;
    m_last_index = -1;
    m_next_index = 0;
    m_read_pos = 0;
    m_skip_to = target;
    m_sample_position = target;
    unparkScanner();
    return true;
}

int ParallelOggFlacReader::readFloats(float *f) {
    std::unique_lock lock(m_mutex);
    int filled = 0;
    const float dc = dcOffset();
    double sum = 0;
    while (filled < (int)m_block_size) {
        auto it = m_decoded.find(m_next_index);
        if (it == m_decoded.end()) {
            if (!m_failure.empty())
                throw std::runtime_error(m_failure);
            if (m_stop || (m_scan_eof && m_next_index > m_last_index))
                return 0; // the end: a partial block is the end, as the readers always report it
            m_cv.wait(lock);
            continue;
        }
        Decoded &run = it->second;
        if (m_read_pos == 0 && m_skip_to >= 0) {
            // The first run after a seek begins at its first frame; skip to the target
            if (run.first_sample >= 0 && run.first_sample <= m_skip_to)
                m_read_pos = std::min(run.samples.size(), (size_t)(m_skip_to - run.first_sample));
            else if (m_log && run.first_sample >= 0)
                m_log->warn(eInput, std::format("FLAC: the run for sample {} begins at {}", m_skip_to, run.first_sample));
            m_skip_to = -1;
        }
        if (m_read_pos >= run.samples.size()) {
            m_decoded.erase(it);
            m_next_index++;
            m_read_pos = 0;
            m_cv.notify_all();
            continue;
        }
        const size_t n = std::min(run.samples.size() - m_read_pos, m_block_size - (size_t)filled);
        const int16_t *s = run.samples.data() + m_read_pos;
        for (size_t i = 0; i < n; i++) {
            const float v = (float)s[i];
            sum += v;
            f[filled + (int)i] = v - dc;
        }
        m_read_pos += n;
        filled += (int)n;
    }
    updateDc(sum / m_block_size);
    m_sample_position += m_block_size;
    return (int)m_block_size;
}
