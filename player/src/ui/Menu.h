// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_MENU_H
#define MUSELD_MENU_H

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "TextEntry.h"

// The on-screen menu: pages of items driven by six keys -- up, down, left,
// right, select, back -- so that it works the same from a keyboard, a game
// controller and the Siri Remote, whose buttons all arrive as those keys.
// Nothing here knows about windows or rendering: the player binds the items
// to its state (PlayerMenu) and an overlay draws lines() (MenuOverlay).
//
// An item is one of: a page (select opens it), a setting (left and right
// step it, select steps it forward; value() names its current state), or an
// action (select runs it).  An item with an `enabled` predicate that says
// false is left out of the page, so a page built once can follow the input:
// NTSC-only settings vanish on a MUSE disc.
struct MenuItem {
    std::string label;
    std::string hint;                    // the key that does the same, shown beside the label
    std::function<std::string()> value;  // the right-hand column; empty for pages and actions
    std::function<void(int)> step;       // settings: -1 for left, +1 for right (and select)
    std::function<void()> activate;      // actions, and pages (set by Menu::addPage)
    std::function<bool()> enabled;       // empty: always shown
    struct MenuPage *page = nullptr;     // a sub page this item opens
};

struct MenuPage {
    std::string title;
    std::vector<MenuItem> items;
    int cursor = 0; // the selected item, as an index into the enabled items
};

enum class MenuKey { eUp, eDown, eLeft, eRight, eSelect, eBack };

// What the overlay draws: one line per enabled item of the open page
struct MenuLine {
    std::string label;
    std::string hint;
    std::string value;
    bool selected = false;
    bool is_page = false;
};

class Menu {
public:
    Menu() = default;
    Menu(const Menu &) = delete;
    Menu &operator=(const Menu &) = delete;

    // The pages are owned here and addressed by pointer; the root is the
    // first one made unless setRoot says otherwise.  Items that open a page are added with addPageItem;
    // a page whose items change (a directory listing) gets them with setItems,
    // and pushPage opens a page from code as select on its item would.
    MenuPage *addPage(std::string title);
    void setRoot(MenuPage *root) { m_root = root; }
    void addPageItem(MenuPage *on, std::string label, MenuPage *opens);
    void setItems(MenuPage *page, std::vector<MenuItem> items);
    void pushPage(MenuPage *page);
    void popPage();

    // A text entry (the browser's address field) takes the keys over while it
    // lasts; its callbacks end it through endTextEntry, which takes effect once
    // the key that caused it has been handled.  type() and backspace() are for
    // a real keyboard's characters.
    void startTextEntry(std::unique_ptr<TextEntry> entry);
    void endTextEntry() { m_end_entry = true; }
    TextEntry *textEntry() const { return m_entry.get(); }
    void type(const std::string &utf8);
    void backspace();
    void acceptEntry();
    void cancelEntry();

    bool isOpen() const { return m_open; }
    void open();
    void close();
    void toggle() { m_open ? close() : open(); }

    // A key while the menu is open.  Returns true when the key was taken;
    // eBack on the root page is not taken (the caller decides what it means
    // there: close on a PC, leave the app on tvOS), nor is any key while closed.
    bool handleKey(MenuKey key);

    // Whether the open page is the root, and which page is open
    bool atRoot() const { return m_stack.size() <= 1; }
    MenuPage *currentPage() const { return m_stack.empty() ? nullptr : m_stack.back(); }

    std::string title() const;
    std::vector<MenuLine> lines() const;

    // A counter that changes whenever what lines() would show may have changed:
    // navigation, a step, open or close.  The overlay redraws when it moves.
    unsigned revision() const { return m_revision; }
    void touch() { m_revision++; }

private:
    std::vector<const MenuItem *> enabledItems(const MenuPage &page) const;
    MenuPage *current() const { return m_stack.empty() ? nullptr : m_stack.back(); }

    std::vector<std::unique_ptr<MenuPage>> m_pages;
    MenuPage *m_root = nullptr;      // the page open() shows; the first page when unset
    std::vector<MenuPage *> m_stack; // the open page last
    std::unique_ptr<TextEntry> m_entry;
    bool m_end_entry = false;
    bool m_open = false;
    unsigned m_revision = 0;
};

#endif //MUSELD_MENU_H
