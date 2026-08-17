// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "Eia608Decoder.h"

#include <algorithm>
#include <bit>
#include <format>
#include <utility>

namespace {

// The basic character set is ASCII with a handful of substitutions
// (CEA-608-E §figure 4).
char32_t basicChar(uint8_t c) {
    switch (c) {
        case 0x2a: return U'á';
        case 0x5c: return U'é';
        case 0x5e: return U'í';
        case 0x5f: return U'ó';
        case 0x60: return U'ú';
        case 0x7b: return U'ç';
        case 0x7c: return U'÷';
        case 0x7d: return U'Ñ';
        case 0x7e: return U'ñ';
        case 0x7f: return U'█';
        default:   return c;
    }
}

// Special characters ({0x11, 0x30-0x3f}); index 9 is the transparent space.
constexpr char32_t c_special_chars[16] = {
    U'®', U'°', U'½', U'¿', U'™', U'¢', U'£', U'♪',
    U'à', U' ', U'è', U'â', U'ê', U'î', U'ô', U'û',
};

// Extended characters ({0x12, 0x20-0x3f} then {0x13, 0x20-0x3f}); they are
// sent after the basic-set approximation of the same character, which they
// replace.
constexpr char32_t c_extended_chars[2][32] = {
    {U'Á', U'É', U'Ó', U'Ú', U'Ü', U'ü', U'‘', U'¡',
     U'*', U'\'', U'—', U'©', U'℠', U'•', U'“', U'”',
     U'À', U'Â', U'Ç', U'È', U'Ê', U'Ë', U'ë', U'Î',
     U'Ï', U'ï', U'Ô', U'Ù', U'ù', U'Û', U'«', U'»'},
    {U'Ã', U'ã', U'Í', U'Ì', U'ì', U'Ò', U'ò', U'Õ',
     U'õ', U'{', U'}', U'\\', U'^', U'_', U'¦', U'~',
     U'Ä', U'ä', U'Ö', U'ö', U'ß', U'¥', U'¤', U'|',
     U'Å', U'å', U'Ø', U'ø', U'┌', U'┐', U'└', U'┘'},
};

void appendUtf8(std::string &s, char32_t cp) {
    if (cp < 0x80) {
        s += (char)cp;
    } else if (cp < 0x800) {
        s += (char)(0xc0 | (cp >> 6));
        s += (char)(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        s += (char)(0xe0 | (cp >> 12));
        s += (char)(0x80 | ((cp >> 6) & 0x3f));
        s += (char)(0x80 | (cp & 0x3f));
    } else {
        s += (char)(0xf0 | (cp >> 18));
        s += (char)(0x80 | ((cp >> 12) & 0x3f));
        s += (char)(0x80 | ((cp >> 6) & 0x3f));
        s += (char)(0x80 | (cp & 0x3f));
    }
}

} // namespace

Eia608Decoder::Eia608Decoder(Logger &log) : m_log(log) {
}

void Eia608Decoder::reset() {
    m_displayed = {};
    m_offscreen = {};
    m_mode = Mode::ePopOn;
    m_rollup_rows = 2;
    m_row = c_rows - 1;
    m_col = 0;
    m_channel = 1;
    m_text_mode = false;
    m_last_control = 0;
    m_dirty = false;
    m_dirty_frames = 0;
    m_last_emitted.clear();
}

Eia608Decoder::Grid &Eia608Decoder::activeGrid() {
    return m_mode == Mode::ePopOn ? m_offscreen : m_displayed;
}

void Eia608Decoder::writeChar(char32_t cp) {
    if (m_col >= c_cols)
        return;
    activeGrid()[m_row][m_col++] = cp;
    if (m_mode != Mode::ePopOn)
        m_dirty = m_wrote_displayed = true;
}

void Eia608Decoder::handlePac(uint8_t b1, uint8_t b2) {
    // Preamble address code: the row from (b1, b2 & 0x20), then either an
    // indent in steps of four columns or a colour/italics style, which does
    // not move the cursor.  Styles and underline are not rendered.
    static constexpr int c_pac_rows[8][2] = {
        {10, 10}, // 0x10 (row 11; the 0x60 variant is unassigned)
        {0, 1},   // 0x11
        {2, 3},   // 0x12
        {11, 12}, // 0x13
        {13, 14}, // 0x14
        {4, 5},   // 0x15
        {6, 7},   // 0x16
        {8, 9},   // 0x17
    };
    m_row = c_pac_rows[b1 - 0x10][(b2 & 0x20) ? 1 : 0];
    if (m_mode == Mode::eRollUp)
        m_row = std::max(m_row, m_rollup_rows - 1); // keep the window on screen
    m_col = (b2 & 0x10) ? ((b2 >> 1) & 7) * 4 : 0;
}

void Eia608Decoder::handleControl(uint8_t b1, uint8_t b2) {
    if (b2 >= 0x40) {
        handlePac(b1, b2);
        return;
    }
    if (b1 == 0x11 && b2 >= 0x20 && b2 <= 0x2f) {
        writeChar(U' '); // mid-row style code: occupies one cell as a space
        return;
    }
    if (b1 == 0x11 && b2 >= 0x30) {
        writeChar(c_special_chars[b2 - 0x30]);
        return;
    }
    if ((b1 == 0x12 || b1 == 0x13) && b2 >= 0x20) {
        // Extended character: replaces the preceding basic-set fallback
        if (m_col > 0)
            m_col--;
        activeGrid()[m_row][m_col] = 0;
        writeChar(c_extended_chars[b1 - 0x12][b2 - 0x20]);
        return;
    }
    if (b1 == 0x17 && b2 >= 0x21 && b2 <= 0x23) {
        m_col = std::min(m_col + (b2 - 0x20), c_cols - 1); // tab offset
        return;
    }
    if (b1 != 0x14)
        return; // field-2 misc codes (0x15) and unassigned codes
    switch (b2) {
        case 0x20: // RCL: resume caption loading (pop-on)
            m_mode = Mode::ePopOn;
            m_text_mode = false;
            break;
        case 0x21: // BS
            if (m_col > 0)
                activeGrid()[m_row][--m_col] = 0;
            break;
        case 0x24: // DER: delete to end of row
            for (int c = m_col; c < c_cols; c++)
                activeGrid()[m_row][c] = 0;
            if (m_mode != Mode::ePopOn)
                m_dirty = true;
            break;
        case 0x25: // RU2/RU3/RU4
        case 0x26:
        case 0x27:
            if (m_mode != Mode::eRollUp) {
                m_displayed = {};
                m_row = c_rows - 1;
                m_dirty = true; // the erase shows once the typing pauses
            }
            m_mode = Mode::eRollUp;
            m_rollup_rows = b2 - 0x23;
            m_text_mode = false;
            m_col = 0;
            break;
        case 0x29: // RDC: resume direct captioning (paint-on)
            m_mode = Mode::ePaintOn;
            m_text_mode = false;
            break;
        case 0x2a: // TR: text restart
        case 0x2b: // RTD: resume text display
            m_text_mode = true;
            break;
        case 0x2c: // EDM: erase displayed memory
            m_displayed = {};
            m_dirty = true;
            break;
        case 0x2d: // CR: roll up the window
            if (m_mode == Mode::eRollUp) {
                const int base = m_row;
                for (int r = base - m_rollup_rows + 1; r < base; r++)
                    m_displayed[std::max(r, 0)] = m_displayed[std::max(r, 0) + 1];
                m_displayed[base] = {};
                m_col = 0;
                m_dirty = true;
            }
            break;
        case 0x2e: // ENM: erase non-displayed memory
            m_offscreen = {};
            break;
        case 0x2f: // EOC: end of caption (flip memories)
            std::swap(m_displayed, m_offscreen);
            m_mode = Mode::ePopOn;
            m_text_mode = false;
            m_dirty = true;
            break;
        default: // AOF/AON/FON and unassigned: nothing to render
            break;
    }
}

std::vector<std::string> Eia608Decoder::gridLines() const {
    std::vector<std::string> lines;
    for (const auto &row : m_displayed) {
        int first = -1, last = -1;
        for (int c = 0; c < c_cols; c++)
            if (row[c] && row[c] != U' ') {
                if (first < 0) first = c;
                last = c;
            }
        if (first < 0)
            continue;
        std::string line;
        for (int c = first; c <= last; c++)
            appendUtf8(line, row[c] ? row[c] : U' ');
        lines.push_back(std::move(line));
    }
    return lines;
}

std::optional<std::vector<std::string>> Eia608Decoder::emitIfChanged() {
    m_dirty = false;
    m_dirty_frames = 0;
    auto lines = gridLines();
    if (lines == m_last_emitted)
        return std::nullopt;
    m_last_emitted = lines;
    return lines;
}

std::optional<std::vector<std::string>> Eia608Decoder::process(uint8_t b1, uint8_t b2) {
    m_wrote_displayed = false;

    const bool parity_ok = (std::popcount(b1) & 1) && (std::popcount(b2) & 1);
    if (!parity_ok && (b1 | b2) != 0) {
        m_log.debug(eDecoder, std::format("CC pair {:02x} {:02x}: parity error", b1, b2));
    } else if (parity_ok) {
        b1 &= 0x7f;
        b2 &= 0x7f;
        if (b1 >= 0x10 && b1 <= 0x1f) {
            const uint16_t pair = (uint16_t)(b1 << 8 | b2);
            if (pair == m_last_control) {
                m_last_control = 0; // the doubled retransmission
            } else {
                m_last_control = pair;
                m_channel = (b1 & 0x08) ? 2 : 1;
                if (m_channel == 1) {
                    handleControl(b1 & ~0x08, b2);
                    // The memory-control events show immediately
                    if (b1 == 0x14 && (b2 == 0x2c || b2 == 0x2d || b2 == 0x2f))
                        return emitIfChanged();
                }
            }
        } else if (b1 >= 0x20) {
            m_last_control = 0;
            if (m_channel == 1 && !m_text_mode) {
                writeChar(basicChar(b1));
                if (b2 >= 0x20)
                    writeChar(basicChar(b2));
            }
        } else if (b1 == 0) {
            m_last_control = 0; // null padding
        }
    }

    // Roll-up and paint-on typing: emit when the typing pauses, or after
    // c_dirty_emit_frames frames of sustained writing.
    if (m_dirty && (!m_wrote_displayed || ++m_dirty_frames >= c_dirty_emit_frames))
        return emitIfChanged();
    return std::nullopt;
}
