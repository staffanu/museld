// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_PREFETCHINGINPUTREADER_H
#define AC3RF_DECODE_PREFETCHINGINPUTREADER_H

#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "InputReader.h"

// Decorator that runs another reader's readFloats on its own thread, keeping a small queue
// of decoded blocks ahead of the consumer.  readFloats then costs one linear copy, so a
// reader whose readFloats is expensive -- the FLAC readers spend milliseconds per block
// decoding -- no longer serializes with the consumer's other work.
//
// seek keeps its relative-to-consumed-samples meaning: the producer is parked at a block
// boundary and the request is adjusted by the amount read ahead before being forwarded, so
// the inner reader must honor forwarded seeks exactly.  Requests reaching before the start
// of the stream are clamped to it, which the consumers' own position bookkeeping assumes.
class PrefetchingInputReader : public InputReader {
public:
    static constexpr int c_default_queue_depth = 3;

    explicit PrefetchingInputReader(std::unique_ptr<InputReader> inner,
                                    int queue_depth = c_default_queue_depth);

    PrefetchingInputReader(const PrefetchingInputReader &) = delete;
    PrefetchingInputReader &operator=(const PrefetchingInputReader &) = delete;
    PrefetchingInputReader(PrefetchingInputReader &&) = delete;
    PrefetchingInputReader &operator=(PrefetchingInputReader &&) = delete;

    ~PrefetchingInputReader() override;

    void initialize() override;
    void seek(off_t no_samples) override;
    int readFloats(float *f) override;
    int bitsPerSample() const override { return m_inner->bitsPerSample(); }

    // The DC estimate must live in the inner reader, where it is applied with one block of
    // lag by the conversion loop; this reader never touches the samples.
    void setDcBlocking(bool enabled) override { m_inner->setDcBlocking(enabled); }

private:
    void produce();

    std::unique_ptr<InputReader> m_inner;
    const int m_queue_depth;

    // Guards all state below.  The producer releases it around the inner readFloats call,
    // so seek can only rely on the queue contents while the producer is parked.
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::vector<float>> m_filled;  // decoded blocks in stream order
    std::vector<std::vector<float>> m_vacant; // recycled block buffers
    off_t m_inner_position = 0; // samples the inner reader has produced, adjusted by seeks
    bool m_eof = false;         // the inner reader reported end of stream; a seek clears it
    bool m_seek_pending = false;
    bool m_producer_parked = false;
    std::exception_ptr m_producer_exception; // rethrown by readFloats once the queue drains
    bool m_stop = false;
    std::thread m_producer;
};

#endif //AC3RF_DECODE_PREFETCHINGINPUTREADER_H
