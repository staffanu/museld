// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_BROWSER_H
#define MUSELD_BROWSER_H

#include <functional>
#include <string>
#include "DirectoryListing.h"
#include "Menu.h"

class SavedState;

// The file browser, as pages of the menu: an "Open" page with an address field
// and the addresses opened recently, and a listing page for the directory being
// browsed, on this machine or on a web server through its index page.  Choosing
// a file (or a udp:// address) calls on_open with the location to play.
class Browser {
public:
    // allow_filesystem: offer this machine's directories (not on tvOS, where
    // the sandbox has nothing to show)
    Browser(Menu &menu, SavedState &state, bool allow_filesystem, std::function<void(const std::string &)> on_open);

    // The "Open" page, to link from the root page or to push as the first page
    MenuPage *openPage() const { return m_open_page; }
    // Lists a directory (pushing the listing page)
    void browse(const std::string &location) { showListing(location); }

    // An address as the user typed it, made into a location: a bare host or
    // host/path gets http://, ~ is the home directory
    static std::string normalizeAddress(const std::string &address);

private:
    void rebuildOpenPage();
    void enterAddress();
    void go(const std::string &location);
    void showListing(const std::string &location);

    Menu &m_menu;
    SavedState &m_state;
    const bool m_allow_filesystem;
    std::function<void(const std::string &)> m_on_open;
    MenuPage *m_open_page;
    MenuPage *m_listing_page;
    std::string m_location;
};

#endif //MUSELD_BROWSER_H
