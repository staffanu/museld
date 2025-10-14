//
// Created by Staffan Ulfberg on 9/26/25.
//

#ifndef AC3RF_DECODE_INPUTREADER_H
#define AC3RF_DECODE_INPUTREADER_H

#include <string.h>
#include <unistd.h>
#include <string>
#include <format>

class InputReader {
public:
    InputReader(int fd, int block_size)
    : m_fd(fd), m_block_size(block_size) {}

    virtual ~InputReader() = default;

    virtual void initialize() = 0;
    virtual int readFloats(float *f) = 0;

    int block_size() const {
        return m_block_size;
    }

protected:
    int m_fd;
    int m_block_size;
};

template <typename T>
class InputReaderImpl : public InputReader {
public:
    InputReaderImpl(int fd, int block_size)
    : InputReader(fd, block_size) {
        m_buffer = new T[block_size];
    }

    InputReaderImpl(const InputReaderImpl &) = delete;
    InputReaderImpl &operator=(const InputReaderImpl &) = delete;
    InputReaderImpl(InputReaderImpl &&) = delete;
    InputReaderImpl &operator=(InputReaderImpl &&) = delete;

    ~InputReaderImpl() override {
        delete[] m_buffer;
    }

    void initialize() override {};

    // reads block_size values and stores them at f.  Returns the number of values actually read.
    int readFloats(float *f) override {
        int filled_bytes = 0;
        ssize_t read_count;
        do {
            read_count = read(m_fd, (uint8_t *)m_buffer + filled_bytes, sizeof(*m_buffer) * m_block_size - filled_bytes);
            if (read_count == -1)
                throw std::runtime_error(std::format("Error reading from file: {}", strerror(errno)));
            filled_bytes += (int)read_count;
        } while (filled_bytes < sizeof(*m_buffer) * m_block_size && read_count > 0);

        for (int i = 0; i < m_block_size; i++)
            f[i] = m_buffer[i];

        return read_count > 0 ? m_block_size : 0;
    }

protected:
    T *m_buffer;
};

#endif //AC3RF_DECODE_INPUTREADER_H
