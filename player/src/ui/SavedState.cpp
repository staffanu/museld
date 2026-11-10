// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include "SavedState.h"

SavedState::SavedState(std::string path) : m_path(std::move(path)) {
    load();
}

void SavedState::load() {
    if (m_path.empty())
        return;
    std::ifstream in(m_path);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "recent" && !value.empty() && m_recent.size() < c_max_recent)
            m_recent.push_back(value);
        else if (key == "last_directory")
            m_last_directory = value;
    }
}

bool SavedState::save() const {
    if (m_path.empty())
        return false;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(m_path).parent_path(), ec);
    std::ofstream out(m_path, std::ios::trunc);
    if (!out)
        return false;
    out << "# museld's saved state, written by the player\n";
    for (const auto &r : m_recent)
        out << "recent=" << r << "\n";
    if (!m_last_directory.empty())
        out << "last_directory=" << m_last_directory << "\n";
    return (bool)out;
}

void SavedState::addRecent(const std::string &address) {
    if (address.empty())
        return;
    m_recent.erase(std::remove(m_recent.begin(), m_recent.end(), address), m_recent.end());
    m_recent.insert(m_recent.begin(), address);
    if (m_recent.size() > c_max_recent)
        m_recent.resize(c_max_recent);
    save();
}

void SavedState::removeRecent(const std::string &address) {
    m_recent.erase(std::remove(m_recent.begin(), m_recent.end(), address), m_recent.end());
    save();
}

void SavedState::setLastDirectory(const std::string &location) {
    m_last_directory = location;
    save();
}
