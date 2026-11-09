// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include "Menu.h"

MenuPage *Menu::addPage(std::string title) {
    m_pages.push_back(std::make_unique<MenuPage>());
    m_pages.back()->title = std::move(title);
    return m_pages.back().get();
}

void Menu::addPageItem(MenuPage *on, std::string label, MenuPage *opens) {
    MenuItem item;
    item.label = std::move(label);
    item.page = opens;
    item.activate = [this, opens] {
        opens->cursor = 0;
        m_stack.push_back(opens);
        m_revision++;
    };
    on->items.push_back(std::move(item));
}

void Menu::open() {
    if (m_pages.empty())
        return;
    m_stack.clear();
    m_stack.push_back(m_pages.front().get());
    m_stack.back()->cursor = 0;
    m_open = true;
    m_revision++;
}

void Menu::close() {
    m_open = false;
    m_stack.clear();
    m_revision++;
}

std::vector<const MenuItem *> Menu::enabledItems(const MenuPage &page) const {
    std::vector<const MenuItem *> items;
    for (const auto &item : page.items)
        if (!item.enabled || item.enabled())
            items.push_back(&item);
    return items;
}

bool Menu::handleKey(MenuKey key) {
    MenuPage *page = current();
    if (!m_open || page == nullptr)
        return false;
    const auto items = enabledItems(*page);
    if (items.empty() && key != MenuKey::eBack)
        return true;
    page->cursor = std::clamp(page->cursor, 0, std::max(0, (int)items.size() - 1));
    const MenuItem *item = items.empty() ? nullptr : items[(size_t)page->cursor];
    switch (key) {
        case MenuKey::eUp:
            page->cursor = page->cursor == 0 ? (int)items.size() - 1 : page->cursor - 1;
            break;
        case MenuKey::eDown:
            page->cursor = page->cursor + 1 >= (int)items.size() ? 0 : page->cursor + 1;
            break;
        case MenuKey::eLeft:
            if (item->step)
                item->step(-1);
            break;
        case MenuKey::eRight:
            if (item->step)
                item->step(+1);
            break;
        case MenuKey::eSelect:
            if (item->activate)
                item->activate();
            else if (item->step)
                item->step(+1);
            break;
        case MenuKey::eBack:
            if (atRoot())
                return false;
            m_stack.pop_back();
            break;
    }
    m_revision++;
    return true;
}

std::string Menu::title() const {
    const MenuPage *page = current();
    return page ? page->title : std::string();
}

std::vector<MenuLine> Menu::lines() const {
    std::vector<MenuLine> lines;
    const MenuPage *page = current();
    if (!m_open || page == nullptr)
        return lines;
    const auto items = enabledItems(*page);
    const int cursor = std::clamp(page->cursor, 0, std::max(0, (int)items.size() - 1));
    for (size_t i = 0; i < items.size(); i++) {
        MenuLine line;
        line.label = items[i]->label;
        line.hint = items[i]->hint;
        line.value = items[i]->value ? items[i]->value() : std::string();
        line.selected = (int)i == cursor;
        line.is_page = items[i]->page != nullptr;
        lines.push_back(std::move(line));
    }
    return lines;
}
