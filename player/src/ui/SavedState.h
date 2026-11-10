// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_SAVEDSTATE_H
#define MUSELD_SAVEDSTATE_H

#include <string>
#include <vector>

// What museld remembers between runs, written by the app itself: the
// addresses opened recently and the directory the browser was last in.  A
// plain text file of key=value lines in the per-user preferences directory
// (SDL_GetPrefPath: the application container on tvOS, ~/.local/share or
// Library/Application Support or AppData elsewhere); MUSELD_STATE_FILE names
// another file, for tests and development.
class SavedState {
public:
    static constexpr size_t c_max_recent = 12;

    // Loads the file if there is one; a missing or unreadable file is an empty state
    explicit SavedState(std::string path = defaultPath());

    const std::string &path() const { return m_path; }
    const std::vector<std::string> &recent() const { return m_recent; }
    const std::string &lastDirectory() const { return m_last_directory; }

    // Puts an address first in the recent list (removing an earlier copy) and saves
    void addRecent(const std::string &address);
    void removeRecent(const std::string &address);
    void setLastDirectory(const std::string &location);

    bool save() const;
    static std::string defaultPath();

private:
    void load();

    std::string m_path;
    std::vector<std::string> m_recent;
    std::string m_last_directory;
};

#endif //MUSELD_SAVEDSTATE_H
