// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_TEXTENTRY_H
#define MUSELD_TEXTENTRY_H

#include <functional>
#include <string>
#include <vector>
#include "Entry.h"

// A line of text being typed, with an on-screen keyboard for the remote: the
// arrows move over the keys, select types the key under the cursor, back
// deletes (and cancels the entry when the text is empty).  A real keyboard
// types straight into it through type() and backspace().  The rows hold the
// characters an address needs; the word keys below them shift between the
// cases, delete a character or the whole line (the field starts with the last
// address, which is long), and end the entry, accepting or cancelling.  Nothing here draws: rows() is what the
// overlay shows.
class TextEntry : public Entry {
public:
    TextEntry(std::string title, std::string initial, std::function<void(const std::string &)> on_done,
              std::function<void()> on_cancel);

    void handleKey(Key key) override;
    void type(const std::string &utf8) override;
    void backspace() override;
    void clear() { m_text.clear(); } // the CLEAR key: start over from an empty line
    void accept() override { if (m_on_done) m_on_done(m_text); }   // a keyboard's Return
    void cancel() override { if (m_on_cancel) m_on_cancel(); }      // a keyboard's Escape

    const std::string &title() const override { return m_title; }
    const std::string &text() const override { return m_text; }
    // The keyboard rows as shown, each a string of keys; the special keys
    // are words.  The cursor is at row() and column() of row(); columns of a
    // row are its keys in order.
    std::vector<std::vector<std::string>> rows() const override;
    int row() const override { return m_row; }
    int column() const override { return m_col; }

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
