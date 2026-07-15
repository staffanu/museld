// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cassert>
#include <chrono>
#include <mutex>
#include <thread>
#include <format>
#include "LdsInputReader.h"

LdsInputReader::LdsInputReader(int fd, uint32_t block_size, bool is_fifo)
    : InputReader(fd, block_size, is_fifo) {
    assert(block_size % 4 == 0);
    m_buffer = new uint8_t[block_size * 5 / 4];
}

LdsInputReader::~LdsInputReader() {
    delete[] m_buffer;
}

void LdsInputReader::initialize() {
}

void LdsInputReader::seek(off_t no_samples) {
    if (m_is_fifo)
        return;
    std::scoped_lock<std::mutex> lock(m_fd_mutex);
    off_t bytes_to_seek = no_samples / 4 * 5;
    lseek(m_fd, bytes_to_seek, SEEK_CUR);
}

int LdsInputReader::readFloats(float *f) {
    size_t total_bytes = m_block_size * 5 / 4;
    size_t filled_bytes = 0;
    while (filled_bytes < total_bytes) {
        ssize_t read_count;
        {
            std::scoped_lock<std::mutex> lock(m_fd_mutex);
            read_count = read(m_fd, m_buffer + filled_bytes, total_bytes - filled_bytes);
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
            return 0;
        }
        filled_bytes += (size_t)read_count;
    }

    for (uint32_t s = 0, d = 0; d < m_block_size; s += 5, d += 4) {
        f[d] = (float)((m_buffer[s] << 2) | (m_buffer[s + 1] >> 6));
        f[d + 1] = (float)(((m_buffer[s + 1] & 0x3f) << 4) | (m_buffer[s + 2] >> 4));
        f[d + 2] = (float)(((m_buffer[s + 2] & 0x0f) << 6) | (m_buffer[s + 3] >> 2));
        f[d + 3] = (float)(((m_buffer[s + 3] & 0x03) << 8) | m_buffer[s + 4]);
    }

    return m_block_size;
}
