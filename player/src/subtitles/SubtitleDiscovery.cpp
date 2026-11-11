// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include "SubtitleDiscovery.h"
#include "ui/DirectoryListing.h"

namespace {

// "capture" for "capture.raw"; a name without an extension is its own stem
std::string stemOf(const std::string &name) {
    const auto dot = name.rfind('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

} // namespace

std::vector<std::pair<std::string, std::string>> matchSubtitleNames(
        const std::string &input_name, const std::vector<std::string> &names) {
    std::vector<std::pair<std::string, std::string>> found;
    const std::string stem = stemOf(input_name);
    for (const auto &name : names) {
        if (name.size() <= 4 || name.compare(name.size() - 4, 4, ".srt") != 0) continue;
        if (name.compare(0, stem.size(), stem) != 0) continue;
        std::string label = name.substr(stem.size(), name.size() - stem.size() - 4);
        while (!label.empty() && label.front() == '.') label.erase(label.begin());
        if (label.empty()) label = "DEFAULT";
        found.emplace_back(std::move(label), name);
    }
    std::sort(found.begin(), found.end(),
              [](const auto &a, const auto &b) { return a.second < b.second; });
    return found;
}

std::vector<SubtitleFile> discoverSubtitleFiles(const std::string &input) {
    std::vector<SubtitleFile> found;
    if (DirectoryListing::isHttp(input)) {
        // The names are matched as the index page shows them (percent-decoded),
        // and fetched by the links it gives; the input is the entry whose link
        // ends the URL, or, when the page does not list it, its name as it
        // stands in the URL
        const auto slash = input.rfind('/');
        if (slash == std::string::npos || slash + 1 >= input.size() || slash < input.find("://") + 3)
            return found;
        const std::string href = input.substr(slash + 1);
        const DirListing listing = DirectoryListing::list(input.substr(0, slash + 1));
        if (!listing.error.empty())
            return found;
        std::string input_name = href;
        std::vector<std::string> names;
        for (const auto &entry : listing.entries) {
            if (entry.is_dir) continue;
            if (entry.href == href) input_name = entry.name;
            names.push_back(entry.name);
        }
        for (auto &[label, name] : matchSubtitleNames(input_name, names))
            for (const auto &entry : listing.entries)
                if (!entry.is_dir && entry.name == name) {
                    found.push_back({std::move(label), DirectoryListing::join(listing.location, entry)});
                    break;
                }
        return found;
    }
    if (DirectoryListing::isUrl(input))
        return found;
    const std::filesystem::path input_path(input);
    std::filesystem::path dir = input_path.parent_path();
    if (dir.empty()) dir = ".";
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        std::error_code entry_ec;
        if (entry.is_regular_file(entry_ec))
            names.push_back(entry.path().filename().string());
    }
    for (auto &[label, name] : matchSubtitleNames(input_path.filename().string(), names))
        found.push_back({std::move(label), (dir / name).string()});
    return found;
}

std::vector<SubtitleEntry> loadSubtitleFile(const std::string &location) {
    if (DirectoryListing::isHttp(location)) {
        std::string body, at;
        const std::string error = DirectoryListing::httpGet(location, false, body, at);
        if (!error.empty())
            throw std::runtime_error("Could not fetch subtitle file: " + error);
        return parseSrtText(body);
    }
    return parseSrt(location);
}
