// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <cstdint>
#include <cassert>
#include <chrono>
#include <mutex>
#include <thread>
#include <format>
#include "LdsInputReader.h"

LdsInputReader::LdsInputReader(std::unique_ptr<ByteSource> source, uint32_t block_size)
    : InputReader(std::move(source), block_size) {
    assert(block_size % 4 == 0);
    m_buffer = new uint8_t[block_size * 5 / 4];
}

LdsInputReader::~LdsInputReader() {
    delete[] m_buffer;
}

void LdsInputReader::initialize() {
}

void LdsInputReader::seek(int64_t no_samples) {
    seekBytes(no_samples / 4 * 5);
}

int LdsInputReader::readFloats(float *f) {
    if (!readFully(m_buffer, m_block_size * 5 / 4))
        return 0;

    // Center the unsigned 10-bit samples on zero; the container's midpoint is
    // exact for this format.  Residual hardware offsets are handled by the
    // adaptive DC blocking in the base class (see setDcBlocking).
    float dc = 512.0f + dcOffset();
    double sum = 0;
    for (uint32_t s = 0, d = 0; d < m_block_size; s += 5, d += 4) {
        uint32_t v0 = (m_buffer[s] << 2) | (m_buffer[s + 1] >> 6);
        uint32_t v1 = ((m_buffer[s + 1] & 0x3f) << 4) | (m_buffer[s + 2] >> 4);
        uint32_t v2 = ((m_buffer[s + 2] & 0x0f) << 6) | (m_buffer[s + 3] >> 2);
        uint32_t v3 = ((m_buffer[s + 3] & 0x03) << 8) | m_buffer[s + 4];
        sum += (double)(v0 + v1 + v2 + v3);
        f[d] = (float)v0 - dc;
        f[d + 1] = (float)v1 - dc;
        f[d + 2] = (float)v2 - dc;
        f[d + 3] = (float)v3 - dc;
    }
    updateDc(sum / m_block_size - 512.0);

    return m_block_size;
}
