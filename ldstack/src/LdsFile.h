// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef LDSTACK_LDSFILE_H
#define LDSTACK_LDSFILE_H

#include <cstdint>
#include <string>
#include <vector>

// Random access to the samples of a Domesday Duplicator .lds capture (10-bit
// unsigned, four samples packed into five bytes), returned as floats centered
// on zero.  A window of decoded samples is kept so that sequential requests
// that advance slowly (one video line at a time) rarely touch the file.
class LdsFile {
public:
    explicit LdsFile(const std::string &path);
    ~LdsFile();
    LdsFile(const LdsFile &) = delete;
    LdsFile &operator=(const LdsFile &) = delete;

    [[nodiscard]] int64_t sampleCount() const { return m_sample_count; }

    // Pointer to `count` consecutive samples starting at `first`, valid until
    // the next call.  Samples outside the file read as zero.
    const float *samples(int64_t first, int64_t count);

private:
    void load(int64_t first, int64_t count);

    int m_fd;
    int64_t m_sample_count;
    std::vector<uint8_t> m_packed;
    std::vector<float> m_window;
    int64_t m_window_first = 0;
    int64_t m_window_count = 0;
    static constexpr int64_t c_window_samples = 1 << 22; // 4M samples, ~0.1 s at 40 MHz
};

#endif //LDSTACK_LDSFILE_H
