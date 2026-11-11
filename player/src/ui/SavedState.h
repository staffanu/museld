// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_SAVEDSTATE_H
#define MUSELD_SAVEDSTATE_H

#include <optional>
#include <string>
#include <utility>
#include <vector>

// What museld remembers between runs, written by the app itself: the
// addresses opened recently and the directory the browser was last in.  A
// plain text file of key=value lines in the per-user preferences directory
// (SDL_GetPrefPath: the application container on tvOS, ~/.local/share or
// Library/Application Support or AppData elsewhere); MUSELD_STATE_FILE names
// another file, for tests and development.
// What a disc (an address) was last played with: the settings that depend on
// the disc rather than the viewer.  Strings as the file holds them; empty is
// "not remembered" (the player's default then)
struct DiscSettings {
    std::string aspect;    // normal, zoom, squeeze, stretch
    std::string overscan;  // full, or the fraction hidden (0, 0.025, 0.05)
    std::string audio;     // default, efm, ac3
    std::string subtitle;  // a track's label, "off", or empty for the default choice
    std::string subtitle2;
    std::string black_level; // auto, m, j (NTSC)
    std::string cx;          // auto, off, on (the analog audio's CX expansion)
    bool operator==(const DiscSettings &) const = default;
};

class SavedState {
public:
    static constexpr size_t c_max_recent = 12;
    static constexpr size_t c_max_discs = 64;

    // Loads the file if there is one; a missing or unreadable file is an empty state
    explicit SavedState(std::string path = defaultPath());

    const std::string &path() const { return m_path; }
    const std::vector<std::string> &recent() const { return m_recent; }
    const std::string &lastDirectory() const { return m_last_directory; }

    // Puts an address first in the recent list (removing an earlier copy) and saves
    void addRecent(const std::string &address);
    void removeRecent(const std::string &address);
    void setLastDirectory(const std::string &location);
    // The settings a disc was last played with, if it was
    std::optional<DiscSettings> discSettings(const std::string &address) const;
    // Records them (first in the list, the oldest beyond c_max_discs dropped) and saves
    void setDiscSettings(const std::string &address, const DiscSettings &settings);

    // Forgets everything and stops saving: the run neither reads nor writes the file (--no-state)
    void disable();
    bool save() const;
    static std::string defaultPath();

private:
    void load();

    std::string m_path;
    std::vector<std::string> m_recent;
    std::string m_last_directory;
    std::vector<std::pair<std::string, DiscSettings>> m_discs; // most recently played first
};

#endif //MUSELD_SAVEDSTATE_H
