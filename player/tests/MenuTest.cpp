// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <catch2/catch_test_macros.hpp>
#include "ui/Menu.h"

namespace {

struct Fixture {
    Menu menu;
    int value = 0;
    int actions = 0;
    bool show_extra = true;
    MenuPage *root, *sub;
    Fixture() {
        root = menu.addPage("ROOT");
        sub = menu.addPage("SUB");
        MenuItem setting;
        setting.label = "Value";
        setting.value = [this] { return std::to_string(value); };
        setting.step = [this](int d) { value += d; };
        root->items.push_back(setting);
        menu.addPageItem(root, "More", sub);
        MenuItem extra;
        extra.label = "Extra";
        extra.enabled = [this] { return show_extra; };
        extra.activate = [this] { actions++; };
        root->items.push_back(extra);
        MenuItem act;
        act.label = "Do";
        act.activate = [this] { actions++; };
        sub->items.push_back(act);
    }
};

} // namespace

TEST_CASE("A closed menu takes no keys and shows nothing", "[Menu]") {
    Fixture f;
    REQUIRE(!f.menu.isOpen());
    REQUIRE(!f.menu.handleKey(MenuKey::eDown));
    REQUIRE(f.menu.lines().empty());
}

TEST_CASE("Opening shows the root; up and down wrap; left and right step a setting", "[Menu]") {
    Fixture f;
    f.menu.open();
    REQUIRE(f.menu.isOpen());
    REQUIRE(f.menu.title() == "ROOT");
    auto lines = f.menu.lines();
    REQUIRE(lines.size() == 3);
    REQUIRE(lines[0].selected);
    REQUIRE(lines[0].value == "0");
    REQUIRE(lines[1].is_page);
    REQUIRE(f.menu.handleKey(MenuKey::eRight));
    REQUIRE(f.value == 1);
    REQUIRE(f.menu.handleKey(MenuKey::eLeft));
    REQUIRE(f.menu.handleKey(MenuKey::eLeft));
    REQUIRE(f.value == -1);
    REQUIRE(f.menu.handleKey(MenuKey::eSelect)); // select steps a setting forward
    REQUIRE(f.value == 0);
    REQUIRE(f.menu.handleKey(MenuKey::eUp));     // wraps to the last
    REQUIRE(f.menu.lines()[2].selected);
    REQUIRE(f.menu.handleKey(MenuKey::eDown));   // and back to the first
    REQUIRE(f.menu.lines()[0].selected);
}

TEST_CASE("Select opens a page, back returns, back on the root is not taken", "[Menu]") {
    Fixture f;
    f.menu.open();
    const unsigned before = f.menu.revision();
    REQUIRE(f.menu.handleKey(MenuKey::eDown));
    REQUIRE(f.menu.handleKey(MenuKey::eSelect));
    REQUIRE(f.menu.title() == "SUB");
    REQUIRE(!f.menu.atRoot());
    REQUIRE(f.menu.lines().size() == 1);
    REQUIRE(f.menu.handleKey(MenuKey::eSelect));
    REQUIRE(f.actions == 1);
    REQUIRE(f.menu.handleKey(MenuKey::eBack));
    REQUIRE(f.menu.title() == "ROOT");
    REQUIRE(f.menu.lines()[1].selected); // the cursor where it was
    REQUIRE(!f.menu.handleKey(MenuKey::eBack));
    REQUIRE(f.menu.isOpen());
    REQUIRE(f.menu.revision() > before);
    f.menu.close();
    REQUIRE(!f.menu.isOpen());
}

TEST_CASE("An item whose predicate says no is left out, and the cursor stays valid", "[Menu]") {
    Fixture f;
    f.menu.open();
    REQUIRE(f.menu.handleKey(MenuKey::eUp)); // on Extra, the last
    REQUIRE(f.menu.lines()[2].label == "Extra");
    f.show_extra = false;
    auto lines = f.menu.lines();
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[1].selected);              // clamped onto the last remaining item
    REQUIRE(f.menu.handleKey(MenuKey::eSelect));
    REQUIRE(f.menu.title() == "SUB");        // which is the page item
}
