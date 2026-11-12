// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_TEXTENTRY_H
#define MUSELD_TEXTENTRY_H

#include <functional>
#include <string>
#include <vector>

// A line of text being typed, with an on-screen keyboard for the remote: the
// arrows move over the keys, select types the key under the cursor, back
// deletes (and cancels the entry when the text is empty).  A real keyboard
// types straight into it through type() and backspace().  The rows hold the
// characters an address needs; the word keys below them shift between the
// cases, delete a character or the whole line (the field starts with the last
// address, which is long), and end the entry, accepting or cancelling.  Nothing here draws: lines() is what the
// overlay shows.
class TextEntry {
public:
    enum class Key { eUp, eDown, eLeft, eRight, eSelect, eBack };

    TextEntry(std::string title, std::string initial, std::function<void(const std::string &)> on_done,
              std::function<void()> on_cancel);

    void handleKey(Key key);
    void type(const std::string &utf8);
    void backspace();
    void clear() { m_text.clear(); } // the CLEAR key: start over from an empty line
    void accept() { if (m_on_done) m_on_done(m_text); }   // a keyboard's Return
    void cancel() { if (m_on_cancel) m_on_cancel(); }      // a keyboard's Escape

    const std::string &title() const { return m_title; }
    const std::string &text() const { return m_text; }
    // The keyboard rows as shown, each a string of keys; the special keys
    // are words.  The cursor is at row() and column() of row(); columns of a
    // row are its keys in order.
    std::vector<std::vector<std::string>> rows() const;
    int row() const { return m_row; }
    int column() const { return m_col; }

private:
    const std::vector<std::string> &currentKeys(int row) const;

    std::string m_title;
    std::string m_text;
    std::function<void(const std::string &)> m_on_done;
    std::function<void()> m_on_cancel;
    bool m_upper = false;
    int m_row = 0, m_col = 0;
};

#endif //MUSELD_TEXTENTRY_H
