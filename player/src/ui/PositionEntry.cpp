// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <cmath>
#include "PositionEntry.h"

PositionEntry::PositionEntry(std::string title, Format format, int digits, double initial,
                             std::function<void(double)> on_done, std::function<void()> on_cancel)
    : m_title(std::move(title)),
      m_format(format),
      // A time is H:MM:SS, hours a single digit (a side plays an hour at most)
      m_digits((size_t)(format == Format::eTime ? 5 : std::max(1, digits)), 0),
      m_on_done(std::move(on_done)),
      m_on_cancel(std::move(on_cancel)) {
    setValue(initial);
    // The cursor starts on the last digit: the units, the ones most often changed
    m_place = (int)m_digits.size() - 1;
}

int PositionEntry::placeLimit(int place) const {
    if (m_format == Format::eTime && (place == 1 || place == 3))
        return 5; // tens of minutes, tens of seconds
    return 9;
}

void PositionEntry::setValue(double value) {
    auto n = (long long)std::llround(std::max(0.0, value));
    if (m_format == Format::eTime) {
        n = std::min(n, 9LL * 3600 + 59 * 60 + 59);
        const int h = (int)(n / 3600), m = (int)(n % 3600 / 60), s = (int)(n % 60);
        m_digits = {h, m / 10, m % 10, s / 10, s % 10};
    } else {
        long long limit = 1;
        for (size_t i = 0; i < m_digits.size(); i++) limit *= 10;
        n = std::min(n, limit - 1);
        for (int i = (int)m_digits.size() - 1; i >= 0; i--) {
            m_digits[(size_t)i] = (int)(n % 10);
            n /= 10;
        }
    }
    render();
}

double PositionEntry::value() const {
    if (m_format == Format::eTime)
        return m_digits[0] * 3600 + (m_digits[1] * 10 + m_digits[2]) * 60 + m_digits[3] * 10 + m_digits[4];
    double n = 0;
    for (int d : m_digits) n = n * 10 + d;
    return n;
}

void PositionEntry::render() {
    m_text.clear();
    for (size_t i = 0; i < m_digits.size(); i++) {
        if (m_format == Format::eTime && (i == 1 || i == 3))
            m_text += ':';
        m_text += (char)('0' + m_digits[i]);
    }
}

int PositionEntry::cursor() const {
    // The digit's position in the text, past the colons before it
    if (m_format == Format::eTime)
        return m_place + (m_place >= 1 ? 1 : 0) + (m_place >= 3 ? 1 : 0);
    return m_place;
}

std::string PositionEntry::hint() const {
    return "UP/DOWN CHANGE  LEFT/RIGHT MOVE  SELECT GO";
}

void PositionEntry::handleKey(Key key) {
    const int n = (int)m_digits.size();
    int &digit = m_digits[(size_t)m_place];
    switch (key) {
        case Key::eUp:
            digit = digit >= placeLimit(m_place) ? 0 : digit + 1;
            render();
            break;
        case Key::eDown:
            digit = digit <= 0 ? placeLimit(m_place) : digit - 1;
            render();
            break;
        case Key::eLeft:
            m_place = m_place == 0 ? n - 1 : m_place - 1;
            break;
        case Key::eRight:
            m_place = m_place + 1 >= n ? 0 : m_place + 1;
            break;
        case Key::eSelect:
            accept();
            break;
        case Key::eBack:
            cancel();
            break;
    }
}

void PositionEntry::type(const std::string &utf8) {
    // Digits only; the rest of a keyboard is ignored.  Each overwrites the
    // digit under the cursor and moves on; at the last digit it stays, so a
    // number typed in full ends on its units.
    if (utf8.size() != 1 || utf8[0] < '0' || utf8[0] > '9')
        return;
    m_digits[(size_t)m_place] = std::min(utf8[0] - '0', placeLimit(m_place));
    render();
    if (m_place + 1 < (int)m_digits.size())
        m_place++;
}

void PositionEntry::backspace() {
    // Back to the digit before, clearing the one left
    m_digits[(size_t)m_place] = 0;
    render();
    if (m_place > 0)
        m_place--;
}
