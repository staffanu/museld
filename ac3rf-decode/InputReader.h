//
// Created by Staffan Ulfberg on 9/26/25.
//

#ifndef AC3RF_DECODE_INPUTREADER_H
#define AC3RF_DECODE_INPUTREADER_H

#include <string.h>
#include <fcntl.h>
#include <string>
#include <format>

class InputReader {
public:
    virtual ~InputReader() = default;

    virtual int readFloats(float *f) = 0;
    virtual int block_size() const = 0;
};

template <typename T>
class InputReaderImpl : public InputReader {
    public:

    InputReaderImpl(int fd, int block_size)
    : m_block_size(block_size),
      m_fd(fd) {
        m_buffer = new T[block_size];
    }

    ~InputReaderImpl() override {
        delete[] m_buffer;
    }

    int block_size() const override {
        return m_block_size;
    }

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

        for (int i = 0; i < filled_bytes; i++)
            f[i] = m_buffer[i];

        return filled_bytes;
    }

protected:
    int m_fd;
    int m_block_size;
    T *m_buffer;
};

#endif //AC3RF_DECODE_INPUTREADER_H
