// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_DIRECTORYLISTING_H
#define MUSELD_DIRECTORYLISTING_H

#include <string>
#include <vector>

// What the file browser lists: a directory on the file system, or a directory
// on a web server through its index page (Apache, nginx, Python's http.server
// and the like all list a directory as a page of links).  Locations are what
// museld opens: a path, or an http:// URL; a udp:// address is not a directory
// and is passed through as is.
struct DirEntry {
    std::string name;   // as shown, percent-decoded
    std::string href;   // what to append to the location to get there
    bool is_dir = false;
    std::string detail; // size or date when the listing gives one, else empty
};

struct DirListing {
    std::vector<DirEntry> entries; // directories first, then files, each sorted by name
    std::string error;             // empty when the listing succeeded
};

namespace DirectoryListing {
    bool isUrl(const std::string &location);
    bool isHttp(const std::string &location);
    // The location with a trailing separator, so that entries can be appended
    std::string withSlash(const std::string &location);
    // A location's parent directory, or the location itself at the root (the
    // file system's root, or the server's)
    std::string parent(const std::string &location);
    // location + entry
    std::string join(const std::string &location, const DirEntry &entry);
    // The index page's links as entries: relative links only (no parent link,
    // no query strings, nothing with a scheme or an absolute path), a trailing
    // slash meaning a directory.  Pure, for the test.
    std::vector<DirEntry> parseHtmlIndex(const std::string &html);
    // Lists a directory or an index page.  Blocks for the fetch.
    DirListing list(const std::string &location);
}

#endif //MUSELD_DIRECTORYLISTING_H
