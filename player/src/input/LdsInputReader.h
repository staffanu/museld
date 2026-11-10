// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_LDSREADER_H
#define AC3RF_DECODE_LDSREADER_H

#include <stdint.h>
#include "InputReader.h"

class LdsInputReader : public InputReader {
public:
    LdsInputReader(std::unique_ptr<ByteSource> source, uint32_t block_size);

    LdsInputReader(const LdsInputReader &) = delete;
    LdsInputReader &operator=(const LdsInputReader &) = delete;
    LdsInputReader(LdsInputReader &&) = delete;
    LdsInputReader &operator=(LdsInputReader &&) = delete;

    ~LdsInputReader() override;

    void initialize() override;
    bool seek(int64_t no_samples) override;
    int readFloats(float *f) override;
    int bitsPerSample() const override { return 10; }
    bool signedSamples() const override { return false; }
    int64_t sampleCount() override {
        const int64_t bytes = m_source ? m_source->size() : -1;
        return bytes < 0 ? -1 : bytes / 5 * 4; // 10-bit samples, four per five bytes
    }

private:
    uint8_t *m_buffer;
};

#endif //AC3RF_DECODE_LDSREADER_H
