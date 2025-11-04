//
// Created by Staffan Ulfberg on 10/10/25.
//

#include "Ac3BlockHandler.h"
#include "../rs/ByteWithErasureFlag.h"
#include "../rs/ReedSolomon.h"

Ac3BlockHandler::Ac3BlockHandler(Logger &log)
: m_log(log),
  rsC1(37, 33, 120, true, true),
  rsC2(36, 32, 120, true, true)
{
}

std::optional<std::array<std::array<uint8_t, 37>, 72>> Ac3BlockHandler::handleFrame(int frame_number, const std::array<uint8_t, 37> &frame) {
    int usedFrameNo;
    if (frame_number != expectedSeq) {
        if (consecutiveInSequence < 3) {
            expectedSeq = 0;
            consecutiveInSequence = 0;
            return std::nullopt; // discard
        } else {
            consecutiveInSequence = 0;
            usedFrameNo = expectedSeq;
        }
    } else {
        usedFrameNo = frame_number;
        consecutiveInSequence++;
    }

    currentBlock[usedFrameNo] = frame;

    if (usedFrameNo == 71) { // block completed
        expectedSeq = 0;
        return std::make_optional(currentBlock);
    } else {
        expectedSeq = usedFrameNo + 1;
        return std::nullopt;
    }
}

std::optional<std::array<std::array<uint8_t, 32>, 66>> Ac3BlockHandler::errorCorrectBlock(const std::array<std::array<uint8_t, 37>, 72> &block) {
    // `block` is interpreted as a matrix of size 36 (rows) x 74 (columns)

    // `c1CorrectedBlock` is a matrix of size 36 x 66
    ByteWithErasureFlag c1CorrectedBlock[36 * 66];

    std::vector<ByteWithErasureFlag> c1Data1;
    std::vector<ByteWithErasureFlag> c1Data2;
    c1Data1.resize(37);
    c1Data2.resize(37);
    for (int k = 0; k < 36; k++) {
        for (int i = 0; i < 37; i++) {
            int i1 = i * 2;
            int i2 = i * 2 + 1;
            c1Data1[i] = ByteWithErasureFlag(block[k * 2 + i1 / 37][i1 % 37]);
            c1Data2[i] = ByteWithErasureFlag(block[k * 2 + i2 / 37][i2 % 37]);
        }
        rsC1.decode(c1Data1);
        rsC1.decode(c1Data2);
        for (int i = 0; i < 33; i++) {
            c1CorrectedBlock[k * 66 + i * 2] = c1Data1[i];
            c1CorrectedBlock[k * 66 + i * 2 + 1] = c1Data2[i];
        }
    }

    // c2CorrectedBlock has the column codewords from c1CorrectedBlock in its rows (with parity bytes dropped)
    // Maybe we could do something if detecting erasures -- like marking the block as bad?
    std::array<std::array<uint8_t, 32>, 66> c2CorrectedBlock;

    std::vector<ByteWithErasureFlag> c2Data;
    c2Data.resize(36);
    for (int k = 0; k < 66; k++) {
        for (int i = 0; i < 36; i++)
            c2Data[i] = c1CorrectedBlock[k + i * 66];
        rsC2.decode(c2Data);
        for (int i = 0; i < 32; i++)
            c2CorrectedBlock[k][i] = c2Data[i].byteValue();
    }

    if (c2CorrectedBlock[0][0] != 0x10 || c2CorrectedBlock[0][1] != 0x00) {
        m_log.warn(eAudio, "Block does not start with 0x10 0x00");
        return std::nullopt;
    }

    return std::make_optional(c2CorrectedBlock);
}

std::vector<std::array<uint8_t, 1536>> Ac3BlockHandler::handleCorrectedBlock(const std::array<std::array<uint8_t, 32>, 66> &block) {
    std::vector<std::array<uint8_t, 1536>> output;

    for (int i = 0; i < 66; i++) {
        // notice we skip the two first bytes in each block
        for (int j = i == 0 ? 2 : 0; j < 32; j++) {
            uint8_t byte = block[i][j];

            if (ac3_output_block_index == 0) {
                if (byte == 0x00) {
                    // just skip
                    continue;
                } else if (byte != 0x0b) {
                    if (seen_first_good_block)
                        m_log.warn(eAudio, "Block does not start with 0x0b");
                    continue;
                }
            } else if (ac3_output_block_index == 1) {
                if (byte != 0x77) {
                    if (seen_first_good_block)
                        m_log.warn(eAudio, "Block does not start with 0x0b 0x77");
                    ac3_output_block_index = 0;
                    continue;
                }
            } else if (ac3_output_block_index == 4) {
                if (byte != 0x1c) {
                    if (seen_first_good_block)
                        m_log.warn(eAudio, "Block does not start with 0x0b 0x77 ? ? 0x1c");
                    ac3_output_block_index = 0;
                    continue;
                }
            }
            if (ac3_output_block_index >= 4)
                seen_first_good_block = true;

            ac3_output_block[ac3_output_block_index++] = byte;

            if (ac3_output_block_index == 1536) {
                output.push_back(ac3_output_block);
                ac3_output_block_index = 0;
            }
        }
    }
    return output;
}

std::string Ac3BlockHandler::reedSolomonStatistics() const {
    return "C1 statistics: " + rsC1.statistics() + "C2 statistics: " + rsC2.statistics();
}
