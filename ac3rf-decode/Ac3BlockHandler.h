//
// Created by Staffan Ulfberg on 10/10/25.
//

#ifndef AC3RF_DECODE_AC3BLOCKHANDLER_H
#define AC3RF_DECODE_AC3BLOCKHANDLER_H

#include <cstdint>
#include <optional>
#include <vector>

#include "rs/ReedSolomon.h"


class Logger;

class Ac3BlockHandler {
public:
    Ac3BlockHandler(Logger &log);
    ~Ac3BlockHandler() = default;
    Ac3BlockHandler(const Ac3BlockHandler &) = delete;
    Ac3BlockHandler(Ac3BlockHandler &&) = delete;
    Ac3BlockHandler &operator=(const Ac3BlockHandler &) = delete;
    Ac3BlockHandler &operator=(Ac3BlockHandler &&) = delete;

    // Keeps track of received frames and returns a block of 72 frames when they have been collcted
    std::optional<std::array<std::array<uint8_t, 37>, 72>> handleFrame(int frame_number, const std::array<uint8_t, 37> &frame);

    // Runs Reed Solomon error correction on a single block, including de-interleaving
    // This method has no state, but cannot be static since it uses the RS decoders and also the logger.
    std::optional<std::array<std::array<uint8_t, 32>, 66>> errorCorrectBlock(const std::array<std::array<uint8_t, 37>, 72> &block);

    // Parses corrected blocks to find the AC3 preample and returns data bursts.
    std::vector<std::array<uint8_t, 1536>> handleCorrectedBlock(const std::array<std::array<uint8_t, 32>, 66> &block);

    std::string reedSolomonStatistics() const;

private:
    Logger &m_log;
    ReedSolomon<0x187, 2> rsC1;
    ReedSolomon<0x187, 2> rsC2;

    // handleFrame state
    std::array<std::array<uint8_t, 37>, 72> currentBlock;
    int expectedSeq = 0;
    int consecutiveInSequence = 0;

    // handleCorrectedBlock state
    bool seen_first_good_block = false;
    int ac3_output_block_index = 0;
    std::array<uint8_t, 1536> ac3_output_block;
};


#endif //AC3RF_DECODE_AC3BLOCKHANDLER_H