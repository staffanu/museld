// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_INPUTREADER_H
#define AC3RF_DECODE_INPUTREADER_H

#include <algorithm>
#include <cstdint>
#include <string.h>
#include <unistd.h>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <format>

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
    InputReader(int fd, uint32_t block_size, bool is_fifo)
    : m_fd(fd), m_block_size(block_size), m_is_fifo(is_fifo) {}

    virtual ~InputReader() = default;

    virtual void initialize() = 0;
    virtual void seek(off_t no_samples) = 0;
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

    bool is_fifo() const {
        return m_is_fifo;
    }

protected:
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

    int m_fd;
    uint32_t m_block_size;
    bool m_is_fifo;
    std::mutex m_fd_mutex;
    bool m_dc_block = false;
    bool m_dc_valid = false;
    double m_dc = 0.0;
};

// Reads exactly m_block_size samples of type T from the file descriptor and converts to float.
// On a fifo we block until the data arrives or the writer closes; on a regular file partial reads
// at end of file are reported as 0 (EOF). EAGAIN is handled with a short sleep to support
// O_NONBLOCK fds.
template <typename T, bool ByteSwap = false>
class InputReaderImpl : public InputReader {
public:
    InputReaderImpl(int fd, uint32_t block_size, bool is_fifo)
    : InputReader(fd, block_size, is_fifo) {
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

    void seek(off_t no_samples) override {
        if (m_is_fifo)
            return;
        std::scoped_lock<std::mutex> lock(m_fd_mutex);
        off_t bytes_to_seek = no_samples * sizeof(*m_buffer);
        // Clamp at the start of the file: without it a backward seek that would
        // land before the start fails outright and the position doesn't move
        off_t current = lseek(m_fd, 0, SEEK_CUR);
        lseek(m_fd, std::max<off_t>(0, current + bytes_to_seek), SEEK_SET);
    }

    int readFloats(float *f) override {
        size_t total_bytes = sizeof(*m_buffer) * m_block_size;
        size_t filled_bytes = 0;
        while (filled_bytes < total_bytes) {
            ssize_t read_count;
            {
                std::scoped_lock<std::mutex> lock(m_fd_mutex);
                read_count = read(m_fd, (uint8_t *)m_buffer + filled_bytes, total_bytes - filled_bytes);
            }
            if (read_count == -1) {
                if (errno == EAGAIN) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                throw std::runtime_error(std::format("Error reading from file: {}", strerror(errno)));
            }
            if (read_count == 0) {
                if (m_is_fifo) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                return 0; // EOF -- signal partial reads as EOF as the original implementation did
            }
            filled_bytes += (size_t)read_count;
        }

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
