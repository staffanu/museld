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
        else if (key == "disc" && m_discs.size() < c_max_discs) {
            // disc=<address>\tfield=value\tfield=value...
            const auto tab = value.find('\t');
            if (tab == std::string::npos || tab == 0)
                continue;
            DiscSettings settings;
            size_t at = tab + 1;
            while (at <= value.size()) {
                const auto end = value.find('\t', at);
                const std::string field = value.substr(at, end == std::string::npos ? std::string::npos : end - at);
                const auto feq = field.find('=');
                if (feq != std::string::npos) {
                    const std::string name = field.substr(0, feq), v = field.substr(feq + 1);
                    if (name == "aspect") settings.aspect = v;
                    else if (name == "overscan") settings.overscan = v;
                    else if (name == "audio") settings.audio = v;
                    else if (name == "subtitle") settings.subtitle = v;
                    else if (name == "subtitle2") settings.subtitle2 = v;
                    else if (name == "black_level") settings.black_level = v;
                    else if (name == "cx") settings.cx = v;
                }
                if (end == std::string::npos) break;
                at = end + 1;
            }
            m_discs.emplace_back(value.substr(0, tab), settings);
        }
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
    for (const auto &[address, d] : m_discs)
        out << "disc=" << address << "\taspect=" << d.aspect << "\toverscan=" << d.overscan << "\taudio=" << d.audio
            << "\tsubtitle=" << d.subtitle << "\tsubtitle2=" << d.subtitle2
            << "\tblack_level=" << d.black_level << "\tcx=" << d.cx << "\n";
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

std::optional<DiscSettings> SavedState::discSettings(const std::string &address) const {
    for (const auto &[a, d] : m_discs)
        if (a == address)
            return d;
    return std::nullopt;
}

void SavedState::setDiscSettings(const std::string &address, const DiscSettings &settings) {
    if (address.empty() || address.find('\t') != std::string::npos)
        return;
    m_discs.erase(std::remove_if(m_discs.begin(), m_discs.end(), [&](const auto &e) { return e.first == address; }), m_discs.end());
    m_discs.insert(m_discs.begin(), {address, settings});
    if (m_discs.size() > c_max_discs)
        m_discs.resize(c_max_discs);
    save();
}

void SavedState::disable() {
    m_path.clear();
    m_recent.clear();
    m_last_directory.clear();
    m_discs.clear();
}
