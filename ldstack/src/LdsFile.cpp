// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "LdsFile.h"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

LdsFile::LdsFile(const std::string &path) {
    m_fd = open(path.c_str(), O_RDONLY);
    if (m_fd < 0)
        throw std::runtime_error("cannot open " + path + ": " + strerror(errno));
    struct stat st{};
    if (fstat(m_fd, &st) != 0)
        throw std::runtime_error("cannot stat " + path);
    m_sample_count = (int64_t)st.st_size / 5 * 4;
}

LdsFile::~LdsFile() {
    close(m_fd);
}

const float *LdsFile::samples(int64_t first, int64_t count) {
    if (first < m_window_first || first + count > m_window_first + m_window_count) {
        // Reload with the request at the start of the window, so a slowly
        // advancing sequence of requests reloads once per window
        load(first, std::max<int64_t>(count, c_window_samples));
    }
    return m_window.data() + (first - m_window_first);
}

void LdsFile::load(int64_t first, int64_t count) {
    // Align to the four-sample packing groups
    int64_t group_first = first / 4;
    if (first < 0)
        group_first = (first - 3) / 4;
    const int64_t group_count = (first + count - group_first * 4 + 3) / 4;
    m_window_first = group_first * 4;
    m_window_count = group_count * 4;
    m_window.assign((size_t)m_window_count, 0.0f);
    m_packed.assign((size_t)group_count * 5, 0);

    // Only the part inside the file is read; the rest stays zero
    const int64_t file_groups = m_sample_count / 4;
    const int64_t g0 = std::max<int64_t>(0, group_first);
    const int64_t g1 = std::min<int64_t>(file_groups, group_first + group_count);
    if (g1 > g0) {
        uint8_t *dst = m_packed.data() + (g0 - group_first) * 5;
        int64_t bytes = (g1 - g0) * 5;
        int64_t offset = g0 * 5;
        while (bytes > 0) {
            ssize_t n = pread(m_fd, dst, (size_t)bytes, (off_t)offset);
            if (n <= 0)
                throw std::runtime_error("read error in .lds file");
            dst += n;
            bytes -= n;
            offset += n;
        }
    }
    const uint8_t *p = m_packed.data();
    float *f = m_window.data();
    for (int64_t g = 0; g < group_count; g++, p += 5, f += 4) {
        const uint32_t v0 = (p[0] << 2) | (p[1] >> 6);
        const uint32_t v1 = ((p[1] & 0x3f) << 4) | (p[2] >> 4);
        const uint32_t v2 = ((p[2] & 0x0f) << 6) | (p[3] >> 2);
        const uint32_t v3 = ((p[3] & 0x03) << 8) | p[4];
        f[0] = (float)v0 - 512.0f;
        f[1] = (float)v1 - 512.0f;
        f[2] = (float)v2 - 512.0f;
        f[3] = (float)v3 - 512.0f;
    }
    // Samples before the start of the file read as zero
    if (group_first < 0)
        std::fill(m_window.begin(), m_window.begin() + (size_t)(-group_first * 4), 0.0f);
}
