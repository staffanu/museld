// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <cctype>
#include "TextEntry.h"

namespace {

// The keyboard: rows of single characters, then a row of words.  Lower and
// upper case share the layout; the shift key swaps them.
const std::vector<std::vector<std::string>> c_rows_lower = {
        {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "_"},
        {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", ".", ":"},
        {"a", "s", "d", "f", "g", "h", "j", "k", "l", "/", "~", "="},
        {"z", "x", "c", "v", "b", "n", "m", ",", "@", "%", "+", "?"},
        {"SHIFT", "SPACE", "DELETE", "CANCEL", "DONE"},
};

std::vector<std::vector<std::string>> upperOf(const std::vector<std::vector<std::string>> &rows) {
    auto out = rows;
    for (auto &row : out)
        for (auto &key : row)
            if (key.size() == 1)
                key[0] = (char)toupper((unsigned char)key[0]);
    return out;
}

const std::vector<std::vector<std::string>> c_rows_upper = upperOf(c_rows_lower);

} // namespace

TextEntry::TextEntry(std::string title, std::string initial, std::function<void(const std::string &)> on_done,
                     std::function<void()> on_cancel)
    : m_title(std::move(title)), m_text(std::move(initial)), m_on_done(std::move(on_done)), m_on_cancel(std::move(on_cancel)) {}

std::vector<std::vector<std::string>> TextEntry::rows() const {
    return m_upper ? c_rows_upper : c_rows_lower;
}

const std::vector<std::string> &TextEntry::currentKeys(int row) const {
    return (m_upper ? c_rows_upper : c_rows_lower)[(size_t)row];
}

void TextEntry::handleKey(Key key) {
    const int n_rows = (int)c_rows_lower.size();
    switch (key) {
        case Key::eUp:
            m_row = (m_row + n_rows - 1) % n_rows;
            break;
        case Key::eDown:
            m_row = (m_row + 1) % n_rows;
            break;
        case Key::eLeft:
            m_col = m_col == 0 ? (int)currentKeys(m_row).size() - 1 : m_col - 1;
            break;
        case Key::eRight:
            m_col = m_col + 1 >= (int)currentKeys(m_row).size() ? 0 : m_col + 1;
            break;
        case Key::eSelect: {
            const std::string &k = currentKeys(m_row)[(size_t)std::min(m_col, (int)currentKeys(m_row).size() - 1)];
            if (k == "SHIFT") m_upper = !m_upper;
            else if (k == "SPACE") type(" ");
            else if (k == "DELETE") backspace();
            else if (k == "CANCEL") { if (m_on_cancel) m_on_cancel(); }
            else if (k == "DONE") { if (m_on_done) m_on_done(m_text); }
            else type(k);
            return;
        }
        case Key::eBack:
            if (m_text.empty()) {
                if (m_on_cancel) m_on_cancel();
            } else {
                backspace();
            }
            return;
    }
    // Moving between rows of different lengths keeps the column in range
    m_col = std::min(m_col, (int)currentKeys(m_row).size() - 1);
}

void TextEntry::type(const std::string &utf8) {
    m_text += utf8;
    if (m_upper && utf8.size() == 1 && isalpha((unsigned char)utf8[0]))
        m_upper = false; // one capital at a time, as phones do
}

void TextEntry::backspace() {
    if (m_text.empty())
        return;
    // Remove one UTF-8 code point
    size_t n = 1;
    while (n < m_text.size() && (m_text[m_text.size() - n] & 0xC0) == 0x80)
        n++;
    m_text.resize(m_text.size() - n);
}
