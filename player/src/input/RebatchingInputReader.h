// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_REBATCHINGINPUTREADER_H
#define AC3RF_DECODE_REBATCHINGINPUTREADER_H

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>
#include "InputReader.h"

// Presents a reader that hands out blocks of one size as a reader of another:
// the player takes over the reader the input probe decoded a live stream
// with (there is no reopening a pipe at the point it has reached), and the
// two have different block sizes.  readFloats fills its block from the inner
// reader's, keeping the remainder for the next call; a partial block at the
// end of the stream is the end, as the readers always report it.
class RebatchingInputReader : public InputReader {
public:
    RebatchingInputReader(std::unique_ptr<InputReader> inner, uint32_t block_size)
        // No source of its own: every byte goes through the inner reader.
        : InputReader(nullptr, block_size),
          m_inner(std::move(inner)),
          m_inner_block(m_inner->block_size()) {}

    RebatchingInputReader(const RebatchingInputReader &) = delete;
    RebatchingInputReader &operator=(const RebatchingInputReader &) = delete;

    // The inner reader is initialized already: it has been reading.
    void initialize() override {}

    bool seek(int64_t no_samples) override {
        // The inner reader is ahead by what is kept back here
        const bool ok = m_inner->seek(no_samples - (int64_t)(m_inner_block.size() - m_inner_read));
        m_inner_read = m_inner_block.size();
        return ok;
    }

    int readFloats(float *f) override {
        size_t filled = 0;
        while (filled < m_block_size) {
            if (m_inner_read == m_inner_block.size()) {
                if (m_inner->readFloats(m_inner_block.data()) == 0)
                    return 0;
                m_inner_read = 0;
            }
            const size_t n = std::min<size_t>(m_block_size - filled, m_inner_block.size() - m_inner_read);
            memcpy(f + filled, m_inner_block.data() + m_inner_read, n * sizeof(float));
            m_inner_read += n;
            filled += n;
        }
        return (int)m_block_size;
    }

    int bitsPerSample() const override { return m_inner->bitsPerSample(); }
    bool signedSamples() const override { return m_inner->signedSamples(); }
    void setDcBlocking(bool enabled) override { m_inner->setDcBlocking(enabled); }
    bool isLive() const override { return m_inner->isLive(); }
    int64_t sampleCount() override { return m_inner->sampleCount(); }

private:
    std::unique_ptr<InputReader> m_inner;
    std::vector<float> m_inner_block;
    size_t m_inner_read = m_inner_block.size(); // nothing kept back yet
};

#endif //AC3RF_DECODE_REBATCHINGINPUTREADER_H
