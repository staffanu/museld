// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_INPUTREADER_H
#define AC3RF_DECODE_INPUTREADER_H

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <string.h>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <format>
#include <stdexcept>
#include "ByteSource.h"

enum InputFormat {
    eUint8,
    eSint8,
    eUint16,
    eSint16,
    eUint16BE,
    eSint16BE,
    eLds,
    eFlac,
    eFlacOgg,
};

class InputReader {
public:
    // The reader owns its source.  PrefetchingInputReader passes none: every
    // byte it hands out comes through the reader it wraps.
    InputReader(std::unique_ptr<ByteSource> source, uint32_t block_size)
    : m_source(std::move(source)), m_block_size(block_size) {}

    virtual ~InputReader() = default;

    virtual void initialize() = 0;
    virtual void seek(int64_t no_samples) = 0;
    virtual int readFloats(float *f) = 0;
    virtual int bitsPerSample() const = 0;

    uint32_t block_size() const {
        return m_block_size;
    }

    // Enable adaptive DC removal on the converted samples.  RF carries no
    // legitimate DC, but the unsigned formats leave their container offset
    // behind with no range convention to derive it from (u16 captures are
    // commonly 10 bits stored in 16-bit values), and capture hardware leaves
    // residual offsets on the signed formats too; an offset comparable to
    // the RF amplitude biases the envelope dropout detector through the
    // analytic bandpass.  The estimate is applied with one block of lag so
    // the conversion loops never re-read their output, which may live in
    // write-combined (GPU staging) memory.  Must stay off for baseband
    // inputs, where absolute levels carry the picture.  Virtual so that
    // PrefetchingInputReader can forward it to the reader it wraps.
    virtual void setDcBlocking(bool enabled) {
        m_dc_block = enabled;
    }

    // Samples arrive as they are produced (fifo, stdin, network stream):
    // seeks are ignored and the player has to keep up.  See ByteSource::isLive.
    virtual bool isLive() const {
        return m_source && m_source->isLive();
    }

protected:
    // Fills `buf` with exactly `total` bytes from the source.  Returns false
    // at the end of the stream (a partial block is reported as the end, as
    // the readers always did).  A live source that has nothing yet is polled
    // every millisecond: its own read already waits a while, and a fifo's
    // O_NONBLOCK read returns at once.
    bool readFully(uint8_t *buf, size_t total) {
        size_t filled = 0;
        while (filled < total) {
            ssize_t count;
            {
                std::scoped_lock<std::mutex> lock(m_source_mutex);
                count = m_source->read(buf + filled, total - filled);
            }
            if (count == -1) {
                if (errno == EAGAIN) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                throw std::runtime_error(std::format("Error reading input: {}", strerror(errno)));
            }
            if (count == 0)
                return false;
            filled += (size_t)count;
        }
        return true;
    }

    // Moves the source by `bytes` relative to its position, clamped at the
    // start (a backward seek that would land before the start otherwise fails
    // outright and the position doesn't move).  Ignored on a live source.
    void seekBytes(int64_t bytes) {
        if (isLive())
            return;
        std::scoped_lock<std::mutex> lock(m_source_mutex);
        int64_t current = m_source->seek(0, SEEK_CUR);
        if (current < 0)
            throw std::runtime_error(std::format("Error seeking input: {}", strerror(errno)));
        m_source->seek(std::max<int64_t>(0, current + bytes), SEEK_SET);
    }

    // No NaN sentinels here: the project compiles with -ffast-math, under
    // which std::isnan constant-folds to false and a NaN would poison every
    // sample that follows.
    float dcOffset() const {
        return m_dc_block && m_dc_valid ? (float)m_dc : 0.0f;
    }

    void updateDc(double block_mean) {
        if (m_dc_block) {
            m_dc = m_dc_valid ? m_dc * 0.9 + block_mean * 0.1 : block_mean;
            m_dc_valid = true;
        }
    }

    std::unique_ptr<ByteSource> m_source;
    uint32_t m_block_size;
    std::mutex m_source_mutex;
    bool m_dc_block = false;
    bool m_dc_valid = false;
    double m_dc = 0.0;
};

// Reads exactly m_block_size samples of type T from the source and converts to float.
// On a live source we wait until the data arrives; on a regular file a partial read at
// the end is reported as 0 (EOF).
template <typename T, bool ByteSwap = false>
class InputReaderImpl : public InputReader {
public:
    InputReaderImpl(std::unique_ptr<ByteSource> source, uint32_t block_size)
    : InputReader(std::move(source), block_size) {
        m_buffer = new T[block_size];
    }

    InputReaderImpl(const InputReaderImpl &) = delete;
    InputReaderImpl &operator=(const InputReaderImpl &) = delete;
    InputReaderImpl(InputReaderImpl &&) = delete;
    InputReaderImpl &operator=(InputReaderImpl &&) = delete;

    ~InputReaderImpl() override {
        delete[] m_buffer;
    }

    void initialize() override {}
    int bitsPerSample() const override { return sizeof(T) * 8; }

    void seek(int64_t no_samples) override {
        seekBytes(no_samples * (int64_t)sizeof(*m_buffer));
    }

    int readFloats(float *f) override {
        if (!readFully((uint8_t *)m_buffer, sizeof(*m_buffer) * m_block_size))
            return 0;

        float dc = dcOffset();
        double sum = 0;
        for (uint32_t i = 0; i < m_block_size; i++) {
            T val = m_buffer[i];
            if constexpr (ByteSwap && sizeof(T) == 2) {
                uint16_t raw;
                memcpy(&raw, &val, 2);
                raw = __builtin_bswap16(raw);
                memcpy(&val, &raw, 2);
            }
            sum += (double)val;
            f[i] = (float)val - dc;
        }
        updateDc(sum / m_block_size);

        return m_block_size;
    }

protected:
    T *m_buffer;
};

#endif //AC3RF_DECODE_INPUTREADER_H
