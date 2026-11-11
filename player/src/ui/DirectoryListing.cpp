// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <system_error>
#include "input/HttpClient.h" // httplib.h with its configuration
#include "DirectoryListing.h"

namespace {

bool startsWith(const std::string &s, const char *prefix) {
    return s.rfind(prefix, 0) == 0;
}

std::string percentDecode(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            out.push_back((char)std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::string htmlUnescape(std::string s) {
    for (const auto &[from, to] : {std::pair{"&amp;", "&"}, std::pair{"&lt;", "<"}, std::pair{"&gt;", ">"},
                                   std::pair{"&quot;", "\""}, std::pair{"&#39;", "'"}, std::pair{"&nbsp;", " "}}) {
        size_t at;
        while ((at = s.find(from)) != std::string::npos)
            s.replace(at, strlen(from), to);
    }
    return s;
}

std::string trim(const std::string &s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return "";
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// The text content of a run of HTML with the tags removed
std::string stripTags(const std::string &s) {
    std::string out;
    bool in_tag = false;
    for (char c : s) {
        if (c == '<') in_tag = true;
        else if (c == '>') in_tag = false;
        else if (!in_tag) out.push_back(c);
    }
    return htmlUnescape(out);
}

void sortEntries(std::vector<DirEntry> &entries) {
    std::stable_sort(entries.begin(), entries.end(), [](const DirEntry &a, const DirEntry &b) {
        if (a.is_dir != b.is_dir)
            return a.is_dir;
        return std::lexicographical_compare(a.name.begin(), a.name.end(), b.name.begin(), b.name.end(),
                                            [](char x, char y) { return tolower((unsigned char)x) < tolower((unsigned char)y); });
    });
}

} // namespace

namespace DirectoryListing {

bool isUrl(const std::string &location) {
    return location.find("://") != std::string::npos;
}

bool isHttp(const std::string &location) {
    return startsWith(location, "http://") || startsWith(location, "https://");
}

std::string withSlash(const std::string &location) {
    if (location.empty() || location.back() == '/')
        return location;
    return location + "/";
}

std::string parent(const std::string &location) {
    std::string s = location;
    while (s.size() > 1 && s.back() == '/')
        s.pop_back();
    const auto slash = s.rfind('/');
    if (isUrl(s)) {
        // Not above the server: http://host/ is the root
        const auto host_slash = s.find('/', s.find("://") + 3);
        if (host_slash == std::string::npos)
            return s + "/";
        if (slash == std::string::npos || slash <= host_slash)
            return s.substr(0, host_slash + 1);
        return s.substr(0, slash + 1);
    }
    if (slash == std::string::npos)
        return s;
    if (slash == 0)
        return "/";
    return s.substr(0, slash + 1);
}

std::string join(const std::string &location, const DirEntry &entry) {
    return withSlash(location) + entry.href;
}

std::vector<DirEntry> parseHtmlIndex(const std::string &html) {
    std::vector<DirEntry> entries;
    size_t at = 0;
    for (;;) {
        // The next <a ... href="...">text</a>
        const size_t a = html.find("<a ", at);
        if (a == std::string::npos)
            break;
        const size_t tag_end = html.find('>', a);
        if (tag_end == std::string::npos)
            break;
        at = tag_end + 1;
        const std::string tag = html.substr(a, tag_end - a);
        size_t h = tag.find("href=");
        if (h == std::string::npos)
            continue;
        h += 5;
        std::string href;
        if (h < tag.size() && (tag[h] == '"' || tag[h] == '\'')) {
            const char quote = tag[h];
            const size_t end = tag.find(quote, h + 1);
            if (end == std::string::npos)
                continue;
            href = tag.substr(h + 1, end - h - 1);
        } else {
            size_t end = h;
            while (end < tag.size() && !isspace((unsigned char)tag[end]))
                end++;
            href = tag.substr(h, end - h);
        }
        const size_t close = html.find("</a>", at);
        const std::string text = close == std::string::npos ? "" : trim(stripTags(html.substr(at, close - at)));
        if (close != std::string::npos)
            at = close + 4;
        // Only links into this directory: relative, no query, no scheme, not
        // the parent, not a sort link (Apache's "?C=N;O=D"), not a fragment
        if (href.empty() || href[0] == '/' || href[0] == '?' || href[0] == '#' || href.find("://") != std::string::npos
            || href == "../" || href == ".." || href == "./")
            continue;
        if (href.find('?') != std::string::npos)
            continue;
        DirEntry entry;
        entry.href = htmlUnescape(href);
        entry.is_dir = entry.href.back() == '/';
        entry.name = percentDecode(entry.is_dir ? entry.href.substr(0, entry.href.size() - 1) : entry.href);
        if (entry.name.empty())
            continue;
        // Apache's table row: the size sits in a later cell of the same row
        if (close != std::string::npos) {
            const size_t row_end = html.find("</tr>", close);
            if (row_end != std::string::npos && row_end - close < 400) {
                const std::string rest = html.substr(close, row_end - close);
                // the cells after the link: date, size, description
                std::vector<std::string> cells;
                size_t c = 0;
                while ((c = rest.find("<td", c)) != std::string::npos) {
                    const size_t cs = rest.find('>', c), ce = rest.find("</td>", c);
                    if (cs == std::string::npos || ce == std::string::npos) break;
                    cells.push_back(trim(stripTags(rest.substr(cs + 1, ce - cs - 1))));
                    c = ce + 5;
                }
                if (cells.size() >= 2 && !entry.is_dir && cells[1] != "-")
                    entry.detail = cells[1];
            }
        }
        (void)text;
        entries.push_back(std::move(entry));
    }
    sortEntries(entries);
    return entries;
}

std::string httpGet(const std::string &url, bool as_directory, std::string &body, std::string &final_url) {
    // The server's redirects are followed (a directory asked for without its
    // slash gets one from Apache, nginx and the like), and where they lead is
    // what the caller is told it got
    std::string at = url;
    for (int hops = 0; ; hops++) {
        const auto scheme_end = at.find("://");
        const auto path_start = at.find('/', scheme_end + 3);
        const std::string origin = at.substr(0, path_start);
        std::string path = path_start == std::string::npos ? "/" : at.substr(path_start);
        if (as_directory)
            path = withSlash(path);
        httplib::Client client(origin);
        client.set_connection_timeout(std::chrono::seconds(5));
        client.set_read_timeout(std::chrono::seconds(10));
        auto response = client.Get(path);
        if (!response)
            return std::format("Cannot reach {}: {}", origin, httplib::to_string(response.error()));
        const int status = response->status;
        if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308)
                && response->has_header("Location") && hops < 5) {
            const std::string target = response->get_header_value("Location");
            if (isUrl(target))
                at = target;
            else if (!target.empty() && target[0] == '/')
                at = origin + target;
            else
                at = origin + path.substr(0, path.rfind('/') + 1) + target;
            continue;
        }
        if (status != 200)
            return std::format("HTTP status {} for {}", status, at);
        final_url = origin + path;
        body = std::move(response->body);
        return {};
    }
}

DirListing list(const std::string &location) {
    DirListing result;
    result.location = withSlash(location);
    if (isHttp(location)) {
        std::string body, at;
        result.error = httpGet(location, true, body, at);
        if (result.error.empty()) {
            result.location = withSlash(at);
            result.entries = parseHtmlIndex(body);
        }
        return result;
    }
    if (isUrl(location)) {
        result.error = "Not a directory";
        return result;
    }
    std::error_code ec;
    for (const auto &item : std::filesystem::directory_iterator(location, std::filesystem::directory_options::skip_permission_denied, ec)) {
        DirEntry entry;
        entry.name = item.path().filename().string();
        if (entry.name.empty() || entry.name[0] == '.')
            continue;
        entry.is_dir = item.is_directory(ec);
        entry.href = entry.is_dir ? entry.name + "/" : entry.name;
        if (!entry.is_dir) {
            const auto size = item.file_size(ec);
            if (!ec)
                entry.detail = size >= (1ull << 30) ? std::format("{:.1f}G", size / 1073741824.0)
                             : size >= (1ull << 20) ? std::format("{:.0f}M", size / 1048576.0)
                             : std::format("{:.0f}K", size / 1024.0);
        }
        result.entries.push_back(std::move(entry));
    }
    if (ec)
        result.error = std::format("Cannot list {}: {}", location, ec.message());
    sortEntries(result.entries);
    return result;
}

} // namespace DirectoryListing
