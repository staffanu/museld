// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_MENU_H
#define MUSELD_MENU_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

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
    // first one made.  Items that open a page are added with addPageItem.
    MenuPage *addPage(std::string title);
    void addPageItem(MenuPage *on, std::string label, MenuPage *opens);

    bool isOpen() const { return m_open; }
    void open();
    void close();
    void toggle() { m_open ? close() : open(); }

    // A key while the menu is open.  Returns true when the key was taken;
    // eBack on the root page is not taken (the caller decides what it means
    // there: close on a PC, leave the app on tvOS), nor is any key while closed.
    bool handleKey(MenuKey key);

    // Whether the open page is the root
    bool atRoot() const { return m_stack.size() <= 1; }

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
    std::vector<MenuPage *> m_stack; // the open page last
    bool m_open = false;
    unsigned m_revision = 0;
};

#endif //MUSELD_MENU_H
