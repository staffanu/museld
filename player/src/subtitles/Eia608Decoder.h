// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_EIA608_DECODER_H
#define MUSELD_EIA608_DECODER_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "logging/Logger.h"

// EIA/CEA-608 closed caption decoder for the byte pairs sliced from NTSC line
// 21 (field 1).  Follows the CC1 service (data channel 1): pop-on, roll-up and
// paint-on captions, the special and extended character sets, and the memory
// control codes.  process() takes one frame's pair with the parity bits intact
// and returns the on-screen lines whenever the displayed caption changes (an
// empty vector when the screen clears).
class Eia608Decoder {
public:
    explicit Eia608Decoder(Logger &log);

    std::optional<std::vector<std::string>> process(uint8_t b1, uint8_t b2);

    // Forget all caption state (after a seek the memories describe the
    // abandoned stream position).
    void reset();

private:
    static constexpr int c_rows = 15;
    static constexpr int c_cols = 32;
    // 0 = empty cell; otherwise a Unicode codepoint
    using Grid = std::array<std::array<char32_t, c_cols>, c_rows>;

    enum class Mode { ePopOn, eRollUp, ePaintOn };

    void handleControl(uint8_t b1, uint8_t b2);
    void handlePac(uint8_t b1, uint8_t b2);
    void writeChar(char32_t cp);
    Grid &activeGrid();
    std::vector<std::string> gridLines() const;
    // Returns the update to emit if the displayed text differs from the last
    // emission, and records it as emitted.
    std::optional<std::vector<std::string>> emitIfChanged();

    Logger &m_log;
    Grid m_displayed{};
    Grid m_offscreen{};
    Mode m_mode = Mode::ePopOn;
    int m_rollup_rows = 2;
    int m_row = c_rows - 1;
    int m_col = 0;
    int m_channel = 1;     // data channel selected by the last control code
    bool m_text_mode = false; // Text service active: its characters are ignored
    // Control codes are transmitted twice; a pair identical to the previous
    // frame's control pair is the retransmission and must be skipped once.
    uint16_t m_last_control = 0;
    // Roll-up and paint-on write into the displayed memory character by
    // character; emission is held back until the typing pauses or this many
    // frames have accumulated, so cues are not one per character.
    static constexpr int c_dirty_emit_frames = 16;
    bool m_dirty = false;
    int m_dirty_frames = 0;
    bool m_wrote_displayed = false; // the pair being processed typed on screen
    std::vector<std::string> m_last_emitted;
};

#endif // MUSELD_EIA608_DECODER_H
