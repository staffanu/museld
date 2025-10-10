//
// Created by Staffan Ulfberg on 10/10/25.
//

#ifndef AC3RF_DECODE_AC3INPUTFRAMING_H
#define AC3RF_DECODE_AC3INPUTFRAMING_H
#include <cstdint>
#include <vector>

class Logger;

class Ac3InputFraming {
public:
    Ac3InputFraming(Logger &log);
    ~Ac3InputFraming() = default;
    Ac3InputFraming(const Ac3InputFraming &) = delete;
    Ac3InputFraming(Ac3InputFraming &&) = delete;
    Ac3InputFraming &operator=(const Ac3InputFraming &) = delete;
    Ac3InputFraming &operator=(Ac3InputFraming &&) = delete;

    std::vector<std::pair<int, std::array<uint8_t, 37>>> arrangeInFrames(const std::vector<uint8_t> &symbols, int number_of_symbols);

private:
    Logger &m_log;

    int frame_number;
    int previous_frame_number = 0;

    int syncFrameSymbolsSeen = 0;
    uint8_t syncFrameNo[4];
    int symbolInFrameCounter = 0;
    uint8_t symbolsInFrame[37 * 4];
    int consecutiveSynched = 0;
    int autoSyncAt = -1;
};


#endif //AC3RF_DECODE_AC3INPUTFRAMING_H