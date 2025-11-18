//
// Created by Staffan Ulfberg on 10/15/25.
//

#include <cassert>
#include "LdsInputReader.h"

LdsInputReader::LdsInputReader(int fd, uint32_t block_size)
    : InputReader(fd, block_size) {
    assert(block_size % 4 == 0);
    m_buffer = new uint8_t[block_size * 5 / 4];
}

LdsInputReader::~LdsInputReader() {
    delete[] m_buffer;
}

void LdsInputReader::initialize() {
}

int LdsInputReader::readFloats(float *f) {
    int filled_bytes = 0;
    ssize_t read_count;
    do {
        read_count = read(m_fd, m_buffer + filled_bytes, m_block_size * 5 / 4 - filled_bytes);
        if (read_count == -1)
            throw std::runtime_error(std::format("Error reading from file: {}", strerror(errno)));
        filled_bytes += (int)read_count;
    } while (filled_bytes < m_block_size * 5 / 4 && read_count > 0);

    for (int s = 0, d = 0; d < m_block_size; s += 5, d += 4) {
        f[d] = (float)((m_buffer[s] << 2) | (m_buffer[s + 1] >> 6));
        f[d + 1] = (float)(((m_buffer[s + 1] & 0x3f) << 4) | (m_buffer[s + 2] >> 4));
        f[d + 2] = (float)(((m_buffer[s + 2] & 0x0f) << 6) | (m_buffer[s + 3] >> 2));
        f[d + 3] = (float)(((m_buffer[s + 3] & 0x03) << 8) | m_buffer[s + 4]);
    }

    return read_count != 0 ? m_block_size : 0;
}
