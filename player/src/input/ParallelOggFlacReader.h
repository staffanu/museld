// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_PARALLELOGGFLACREADER_H
#define AC3RF_DECODE_PARALLELOGGFLACREADER_H

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "InputReader.h"

class Logger;

// Reads a FLAC-in-Ogg capture (.ldf) decoding on several threads.  libFLAC decodes
// one stream in series, and on an Apple TV 4K's A10X one core decodes a 10-bit
// 62.5 MHz capture at 0.88x real time.  FLAC frames are independent, and the Ogg
// pages say where they start: a page whose first packet is not a continuation
// begins a frame.  So a scanner thread cuts the stream into runs of pages that
// begin at such a page, and decoder threads take the runs in turn, each with its
// own libFLAC decoder that is given the stream's header pages and then the run,
// with the page sequence numbers rewritten to be consecutive so libFLAC sees a
// well-formed stream every time.  The decoded runs come back by index and
// readFloats hands them out in order; each frame carries its sample number, so a
// run's first sample is known and a seek lands where it says.
//
// Seeking bisects the stream by the pages' granule positions (the sample count at
// a page's end), so it does not depend on STREAMINFO's 36-bit sample count, which
// long captures wrap.  Not for live sources: the stream must be seekable.
class ParallelOggFlacReader : public InputReader {
public:
    // `threads` decoder threads (at least 1; one gives the serial decoder with the
    // scanner's overhead, so the factory uses this class from 2 up)
    ParallelOggFlacReader(std::unique_ptr<ByteSource> source, uint32_t block_size, int threads, Logger *log = nullptr);

    ParallelOggFlacReader(const ParallelOggFlacReader &) = delete;
    ParallelOggFlacReader &operator=(const ParallelOggFlacReader &) = delete;

    ~ParallelOggFlacReader() override;

    void initialize() override;
    bool seek(int64_t no_samples) override;
    int readFloats(float *f) override;
    int bitsPerSample() const override { return m_bits_per_sample; }
    bool signedSamples() const override { return true; }
    int64_t sampleCount() override { return m_sample_count; }

private:
    class RunDecoder;

    struct Run {
        uint64_t generation;
        int64_t index;
        std::vector<uint8_t> bytes; // whole pages, renumbered to follow the header pages
    };
    struct Decoded {
        int64_t first_sample = -1;  // of the run's first frame
        std::vector<int16_t> samples;
        bool ok = true;
        std::string error;
    };

    void scan();
    void decode(int which);
    bool readPagesFrom(int64_t offset, std::vector<uint8_t> &buf, size_t wanted);
    int64_t pageStartAfter(int64_t offset, int64_t limit);
    int64_t findRunStartFor(int64_t target_sample);
    void parkScanner(std::unique_lock<std::mutex> &lock);
    void unparkScanner();

    Logger *m_log;
    const int m_threads;
    int m_bits_per_sample = 0;
    int m_fixed_blocksize = 0;
    int64_t m_sample_count = -1;
    int64_t m_data_start = 0;             // byte offset of the first audio page
    std::vector<uint8_t> m_headers;       // the header pages, as libFLAC needs them first
    uint32_t m_header_pages = 0;          // how many, for the renumbering
    uint32_t m_serial = 0;

    // Everything below under m_mutex (m_source_mutex of the base is not used: the
    // scanner thread is the only reader of the source while it runs, and seek()
    // parks it before touching the source)
    std::mutex m_mutex;
    std::condition_variable m_cv;
    uint64_t m_generation = 0;
    int64_t m_scan_offset = 0;            // where the scanner continues
    int64_t m_scan_next_index = 0;
    bool m_scan_eof = false;
    int64_t m_last_index = -1;            // the last run's index once the end is known
    bool m_scanner_park_request = false;
    bool m_scanner_parked = false;
    std::deque<Run> m_runs;               // scanned, waiting for a decoder
    int m_decoding = 0;                   // runs being decoded
    std::map<int64_t, Decoded> m_decoded; // decoded, by index
    int64_t m_next_index = 0;             // the run readFloats takes next
    size_t m_read_pos = 0;                // within that run's samples
    int64_t m_skip_to = -1;               // after a seek: the sample the reader wants
    int64_t m_sample_position = 0;
    std::string m_failure;
    bool m_stop = false;
    std::vector<std::thread> m_decoders;
    std::thread m_scanner; // last: joined before the state above goes away
};

#endif //AC3RF_DECODE_PARALLELOGGFLACREADER_H
