// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_ENTRY_H
#define MUSELD_ENTRY_H

#include <string>
#include <vector>

// Something the menu lets the user fill in, taking the six keys over while
// it lasts: a line of text with an on-screen keyboard (TextEntry, the
// browser's address field) or a number with a cursor on one of its digits
// (PositionEntry, the player's go-to).  A real keyboard types into either
// through type() and backspace(), and its Return and Escape are accept()
// and cancel().  Nothing here draws: the overlay shows the title and the
// text, and the keyboard rows if there are any.
class Entry {
public:
    enum class Key { eUp, eDown, eLeft, eRight, eSelect, eBack };

    virtual ~Entry() = default;

    virtual void handleKey(Key key) = 0;
    virtual void type(const std::string &utf8) = 0;
    virtual void backspace() = 0;
    virtual void accept() = 0;
    virtual void cancel() = 0;

    [[nodiscard]] virtual const std::string &title() const = 0;
    [[nodiscard]] virtual const std::string &text() const = 0;
    // The character of text() the cursor is on; -1 is the end of the line
    [[nodiscard]] virtual int cursor() const { return -1; }
    // The on-screen keyboard as shown, each row a string of keys (the special
    // keys are words), with the cursor at row() and column(); no rows for an
    // entry without a keyboard
    [[nodiscard]] virtual std::vector<std::vector<std::string>> rows() const { return {}; }
    [[nodiscard]] virtual int row() const { return 0; }
    [[nodiscard]] virtual int column() const { return 0; }
};

#endif //MUSELD_ENTRY_H
