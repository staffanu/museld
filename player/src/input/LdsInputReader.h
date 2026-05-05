// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_LDSREADER_H
#define AC3RF_DECODE_LDSREADER_H

#include <stdint.h>
#include "InputReader.h"

class LdsInputReader : public InputReader {
public:
    LdsInputReader(int fd, uint32_t block_size, bool is_fifo);

    LdsInputReader(const LdsInputReader &) = delete;
    LdsInputReader &operator=(const LdsInputReader &) = delete;
    LdsInputReader(LdsInputReader &&) = delete;
    LdsInputReader &operator=(LdsInputReader &&) = delete;

    ~LdsInputReader() override;

    void initialize() override;
    void seek(off_t no_samples) override;
    int readFloats(float *f) override;

private:
    uint8_t *m_buffer;
};

#endif //AC3RF_DECODE_LDSREADER_H
