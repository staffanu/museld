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

void Menu::setItems(MenuPage *page, std::vector<MenuItem> items) {
    page->items = std::move(items);
    page->cursor = 0;
    m_revision++;
}

void Menu::pushPage(MenuPage *page) {
    if (!m_open)
        open();
    page->cursor = 0;
    m_stack.push_back(page);
    m_revision++;
}

void Menu::popPage() {
    if (m_stack.size() > 1)
        m_stack.pop_back();
    m_revision++;
}

void Menu::startTextEntry(std::unique_ptr<TextEntry> entry) {
    m_entry = std::move(entry);
    m_end_entry = false;
    m_revision++;
}

void Menu::type(const std::string &utf8) {
    if (m_entry) {
        m_entry->type(utf8);
        m_revision++;
    }
}

void Menu::backspace() {
    if (m_entry) {
        m_entry->backspace();
        m_revision++;
    }
}

void Menu::acceptEntry() {
    if (!m_entry)
        return;
    m_entry->accept();
    if (m_end_entry) {
        m_entry.reset();
        m_end_entry = false;
    }
    m_revision++;
}

void Menu::cancelEntry() {
    if (!m_entry)
        return;
    m_entry->cancel();
    if (m_end_entry) {
        m_entry.reset();
        m_end_entry = false;
    }
    m_revision++;
}

void Menu::open() {
    if (m_pages.empty())
        return;
    m_stack.clear();
    m_stack.push_back(m_root != nullptr ? m_root : m_pages.front().get());
    m_stack.back()->cursor = 0;
    m_open = true;
    m_revision++;
}

void Menu::close() {
    m_open = false;
    m_stack.clear();
    m_entry.reset();
    m_end_entry = false;
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
    if (m_entry) {
        static const TextEntry::Key keys[] = {TextEntry::Key::eUp, TextEntry::Key::eDown, TextEntry::Key::eLeft,
                                             TextEntry::Key::eRight, TextEntry::Key::eSelect, TextEntry::Key::eBack};
        m_entry->handleKey(keys[(int)key]);
        if (m_end_entry) {
            m_entry.reset();
            m_end_entry = false;
        }
        m_revision++;
        return true;
    }
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
            // A copy is called: activating may replace the page's items (the
            // browser's pages are rebuilt by their own items)
            if (item->activate) {
                const auto activate = item->activate;
                activate();
            } else if (item->step) {
                const auto step = item->step;
                step(+1);
            }
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
