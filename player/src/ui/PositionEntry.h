// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_POSITIONENTRY_H
#define MUSELD_POSITIONENTRY_H

#include <functional>
#include <string>
#include <vector>
#include "Entry.h"

// A number being entered the way a clock is set: the cursor sits on one
// digit, up and down change it (wrapping within what the place allows, the
// tens of seconds stop at 5), left and right move between the digits,
// select accepts and back cancels.  It starts from where playback is, so a
// jump nearby changes a digit or two.  A keyboard's digits overwrite the
// digit under the cursor and move on, as a VCR's do; its Backspace moves
// back.  The number is a count (a chapter or frame number, a fixed number
// of digits) or a time, H:MM:SS.
class PositionEntry : public Entry {
public:
    enum class Format { eNumber, eTime };

    // digits: the places of an eNumber; the value is clamped to what they
    // hold.  on_done gets the number, or the time in seconds.
    PositionEntry(std::string title, Format format, int digits, double initial,
                  std::function<void(double)> on_done, std::function<void()> on_cancel);

    void handleKey(Key key) override;
    void type(const std::string &utf8) override;
    void backspace() override;
    void accept() override { if (m_on_done) m_on_done(value()); }
    void cancel() override { if (m_on_cancel) m_on_cancel(); }

    [[nodiscard]] const std::string &title() const override { return m_title; }
    [[nodiscard]] const std::string &text() const override { return m_text; }
    [[nodiscard]] int cursor() const override;

    [[nodiscard]] double value() const;
    [[nodiscard]] Format format() const { return m_format; }

private:
    void setValue(double value);
    void render();
    [[nodiscard]] int placeLimit(int place) const; // the largest digit the place takes

    const std::string m_title;
    const Format m_format;
    std::vector<int> m_digits; // most significant first
    int m_place = 0;           // the cursor's digit
    std::string m_text;
    std::function<void(double)> m_on_done;
    std::function<void()> m_on_cancel;
};

#endif //MUSELD_POSITIONENTRY_H
