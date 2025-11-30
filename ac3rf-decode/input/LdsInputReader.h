//
// Created by Staffan Ulfberg on 10/15/25.
//

#ifndef AC3RF_DECODE_LDSREADER_H
#define AC3RF_DECODE_LDSREADER_H

#include <stdint.h>
#include "InputReader.h"

class LdsInputReader : public InputReader {
public:
    LdsInputReader(int fd, uint32_t block_size);

    LdsInputReader(const LdsInputReader &) = delete;
    LdsInputReader &operator=(const LdsInputReader &) = delete;
    LdsInputReader(LdsInputReader &&) = delete;
    LdsInputReader &operator=(LdsInputReader &&) = delete;

    ~LdsInputReader() override;

    void initialize() override;
    int readFloats(float *f) override;

private:
    uint8_t *m_buffer;
};

#endif //AC3RF_DECODE_LDSREADER_H