// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cstdlib>
#include <filesystem>
#include <format>
#include <thread>
#include "Browser.h"
#include "SavedState.h"
#include "input/InputReaderFactory.h"

namespace {

std::string homeDirectory() {
    if (const char *home = getenv("HOME"); home != nullptr && *home)
        return home;
    if (const char *profile = getenv("USERPROFILE"); profile != nullptr && *profile)
        return profile;
    return "/";
}

// A location's tail, for a title that fits
std::string shortLocation(const std::string &location, size_t max) {
    if (location.size() <= max)
        return location;
    return "..." + location.substr(location.size() - max + 3);
}

bool looksLikeDirectory(const std::string &location) {
    if (location.empty() || location.back() == '/')
        return true;
    if (DirectoryListing::isUrl(location)) {
        // On a web server only the name tells: a capture's extension is a file,
        // anything else is taken for a directory (http://host alone is the root)
        if (!DirectoryListing::isHttp(location))
            return false;
        const auto after_scheme = location.find("://") + 3;
        if (location.find('/', after_scheme) == std::string::npos)
            return true;
        const std::string name = location.substr(location.rfind('/') + 1);
        return !inputFormatFromFilename(name).has_value();
    }
    std::error_code ec;
    return std::filesystem::is_directory(location, ec);
}

} // namespace

Browser::Browser(Menu &menu, SavedState &state, bool allow_filesystem, std::function<void(const std::string &)> on_open)
    : m_menu(menu), m_state(state), m_allow_filesystem(allow_filesystem), m_on_open(std::move(on_open)) {
    m_open_page = m_menu.addPage("OPEN");
    m_listing_page = m_menu.addPage("");
    rebuildOpenPage();
}

std::string Browser::normalizeAddress(const std::string &address) {
    std::string a = address;
    // trim
    while (!a.empty() && (a.back() == ' ' || a.back() == '\t')) a.pop_back();
    size_t b = 0;
    while (b < a.size() && (a[b] == ' ' || a[b] == '\t')) b++;
    a = a.substr(b);
    if (a.empty())
        return a;
    if (a[0] == '~')
        return homeDirectory() + a.substr(1);
    if (DirectoryListing::isUrl(a) || a[0] == '/' || a[0] == '.' || (a.size() > 1 && a[1] == ':'))
        return a;
    return "http://" + a;
}

void Browser::rebuildOpenPage() {
    std::vector<MenuItem> items;
    MenuItem enter;
    enter.label = "Enter an address...";
    enter.activate = [this] { enterAddress(); };
    items.push_back(std::move(enter));
    if (!m_state.lastDirectory().empty()) {
        MenuItem last;
        last.label = "Browse " + shortLocation(m_state.lastDirectory(), 40);
        last.activate = [this, location = m_state.lastDirectory()] { showListing(location); };
        items.push_back(std::move(last));
    }
    if (m_allow_filesystem) {
        MenuItem home;
        home.label = "Browse this computer";
        home.activate = [this] { showListing(homeDirectory()); };
        items.push_back(std::move(home));
    }
    for (const auto &address : m_state.recent()) {
        MenuItem item;
        item.label = shortLocation(address, 56);
        item.activate = [this, address] { go(address); };
        items.push_back(std::move(item));
    }
    m_menu.setItems(m_open_page, std::move(items));
}

void Browser::enterAddress() {
    // Prefilled with what was typed last (so a failed address can be corrected),
    // else the most recent address, else the scheme
    const std::string initial = !m_last_typed.empty() ? m_last_typed
                              : m_state.recent().empty() ? std::string("http://") : m_state.recent().front();
    m_menu.startTextEntry(std::make_unique<TextEntry>(
            "ADDRESS", initial,
            [this](const std::string &text) {
                m_menu.endTextEntry();
                m_last_typed = text;
                const std::string location = normalizeAddress(text);
                if (!location.empty())
                    go(location);
            },
            [this] { m_menu.endTextEntry(); }));
}

// Plays a file or stream, lists a directory.  The argument is usually the
// capture of the item's lambda that is running, and both pages are rebuilt
// below, which destroys that item: the location is copied before anything
// else.
void Browser::go(const std::string &location_of_item) {
    const std::string location = location_of_item;
    if (location.rfind("udp://", 0) == 0 || !looksLikeDirectory(location)) {
        m_state.addRecent(location);
        rebuildOpenPage();
        if (m_on_open)
            m_on_open(location);
        return;
    }
    showListing(location);
}

void Browser::showListing(const std::string &location_of_item) {
    const std::string location = location_of_item; // see go()
    m_location = DirectoryListing::withSlash(location);
    m_listing_page->title = shortLocation(m_location, 60);
    if (!DirectoryListing::isHttp(m_location)) {
        m_fetch.reset(); // a fetch still under way is for a listing no longer shown
        applyListing(DirectoryListing::list(m_location));
        return;
    }
    // An index page takes up to 15 s from a slow or absent server (the client's
    // timeouts), too long to freeze the picture for: fetched in the background,
    // the page says so meanwhile, and poll() fills it in
    m_fetch = std::make_shared<Fetch>();
    m_fetch->location = m_location;
    std::thread([fetch = m_fetch] {
        fetch->result = DirectoryListing::list(fetch->location);
        fetch->done.store(true, std::memory_order_release);
    }).detach();
    std::vector<MenuItem> items;
    MenuItem wait;
    wait.label = "Listing...";
    items.push_back(std::move(wait));
    m_menu.setItems(m_listing_page, std::move(items));
    pushListingPage();
}

void Browser::poll() {
    if (!m_fetch || !m_fetch->done.load(std::memory_order_acquire))
        return;
    const std::shared_ptr<Fetch> fetch = std::move(m_fetch);
    if (fetch->location == m_location) // not navigated away from meanwhile
        applyListing(fetch->result);
}

void Browser::applyListing(const DirListing &listing) {
    if (!listing.location.empty()) {
        m_location = listing.location; // where the server's redirects led
        m_listing_page->title = shortLocation(m_location, 60);
    }
    std::vector<MenuItem> items;
    if (!listing.error.empty()) {
        MenuItem error;
        error.label = shortLocation(listing.error, 70);
        error.activate = [this] { showListing(m_location); }; // select retries
        items.push_back(std::move(error));
    } else {
        m_state.setLastDirectory(m_location);
        if (DirectoryListing::parent(m_location) != m_location) {
            MenuItem up;
            up.label = "..";
            up.value = [] { return std::string("UP"); }; // in the value column, where it does not shift the labels
            up.activate = [this] { showListing(DirectoryListing::parent(m_location)); };
            items.push_back(std::move(up));
        }
        for (const auto &entry : listing.entries) {
            MenuItem item;
            item.label = entry.is_dir ? entry.name + "/" : entry.name;
            if (!entry.detail.empty())
                item.value = [detail = entry.detail] { return detail; };
            const std::string target = DirectoryListing::join(m_location, entry);
            if (entry.is_dir)
                item.activate = [this, target] { showListing(target); };
            else
                item.activate = [this, target] { go(target); };
            items.push_back(std::move(item));
        }
        if (listing.entries.empty()) {
            MenuItem empty;
            empty.label = "(empty)";
            items.push_back(std::move(empty));
        }
    }
    m_menu.setItems(m_listing_page, std::move(items));
    pushListingPage();
}

// From the Open page the listing is pushed; within a listing its items change
void Browser::pushListingPage() {
    if (m_menu.currentPage() != m_listing_page)
        m_menu.pushPage(m_listing_page);
}
