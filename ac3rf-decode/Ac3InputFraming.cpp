//
// Created by Staffan Ulfberg on 10/10/25.
//

#include <format>
#include "Logger.h"
#include "Ac3InputFraming.h"

Ac3InputFraming::Ac3InputFraming(Logger &log)
: m_log(log)
{
}

// Finds the sync pattens and breaks the sequence up into frames
std::vector<std::pair<int, std::array<uint8_t, 37>>> Ac3InputFraming::arrangeInFrames(const std::vector<uint8_t> &symbols, int number_of_symbols) {
    std::vector<std::pair<int, std::array<uint8_t, 37>>> frames;

    for (int i = 0; i < number_of_symbols; i++) {
        uint8_t s = symbols[i];
        if (syncFrameSymbolsSeen < 12 && autoSyncAt < i) {
            bool is_next_sync_symbol;
            switch (syncFrameSymbolsSeen) {
                case 0:
                    is_next_sync_symbol = s == 0;
                    break;
                case 1:
                    is_next_sync_symbol = s == 1;
                    break;
                case 2:
                    is_next_sync_symbol = s == 1;
                    break;
                case 3:
                    is_next_sync_symbol = s == 3;
                    break;
                case 4:
                case 5:
                case 6:
                case 7:
                    syncFrameNo[syncFrameSymbolsSeen - 4] = s;
                    is_next_sync_symbol = true;
                    break;
                case 8:
                case 9:
                case 10:
                case 11:
                    is_next_sync_symbol = s == 0;
                    break;
                default: throw std::runtime_error("cannot happen");
            }
            if (is_next_sync_symbol) {
                syncFrameSymbolsSeen += 1;
                if (syncFrameSymbolsSeen == 12)
                    frame_number = (syncFrameNo[0] << 6) | (syncFrameNo[1] << 4) | (syncFrameNo[2] << 2) | syncFrameNo[3];
            } else {
                if (consecutiveSynched > 0) {
                    m_log.debug(std::format("Missing sync index {} (consecutiveSynched={})!", i, consecutiveSynched));
                    consecutiveSynched -= 1;
                    frame_number = (previous_frame_number + 1) % 72;
                    autoSyncAt = i + 12 - syncFrameSymbolsSeen;
                } else {
                    syncFrameSymbolsSeen = 0;
                }
            }
        } else if (i >= autoSyncAt) {
            if (syncFrameSymbolsSeen == 12 && symbolInFrameCounter == 0)
                consecutiveSynched = std::min(consecutiveSynched + 1, 7);
            else if (i == autoSyncAt) {
                syncFrameSymbolsSeen = 12;
                autoSyncAt = -1; // FIXME: better logic this is broken!
            }

            symbolsInFrame[symbolInFrameCounter] = s;
            symbolInFrameCounter += 1;
            if (symbolInFrameCounter == 37 * 4) {
                std::array<uint8_t, 37> frame;
                for (int j = 0; j < 37; j++)
                    frame[j] = symbolsInFrame[j * 4] << 6 | symbolsInFrame[j * 4 + 1] << 4 | symbolsInFrame[j * 4 + 2] << 2 | symbolsInFrame[j * 4 + 3];

                frames.push_back(std::make_pair(frame_number, frame));

                previous_frame_number = frame_number;
                symbolInFrameCounter = 0;
                syncFrameSymbolsSeen = 0;
            }
        }
    }
    return frames;
}
