// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <catch2/catch_test_macros.hpp>
#include "ui/Menu.h"
#include "ui/PositionEntry.h"
#include "ui/TextEntry.h"

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

TEST_CASE("The root is the page setRoot names, whatever was made first", "[Menu]") {
    Menu menu;
    MenuPage *first = menu.addPage("FIRST");
    MenuPage *root = menu.addPage("ROOT");
    (void)first;
    menu.setRoot(root);
    menu.open();
    REQUIRE(menu.title() == "ROOT");
    REQUIRE(menu.atRoot());
}

TEST_CASE("A text entry whose callback closes the menu is destroyed after the call, not inside it", "[Menu]") {
    // The browser's address field: DONE opens the file, and opening closes the
    // menu, from inside the entry's own on_done
    Fixture f;
    f.menu.open();
    std::string done_with;
    f.menu.startEntry(std::make_unique<TextEntry>(
            "ADDRESS", "x",
            [&](const std::string &text) {
                f.menu.endEntry();
                done_with = text;
                f.menu.close();
                done_with += "/closed"; // still inside the callback: its captures must be alive
            },
            [&] { f.menu.endEntry(); }));
    REQUIRE(f.menu.entry() != nullptr);
    SECTION("from a keyboard's Return") {
        f.menu.acceptEntry();
    }
    SECTION("from the on-screen DONE key") {
        f.menu.handleKey(MenuKey::eUp); // the last row: SHIFT SPACE DELETE CLEAR CANCEL DONE
        for (int i = 0; i < 5; i++)
            f.menu.handleKey(MenuKey::eRight);
        f.menu.handleKey(MenuKey::eSelect);
    }
    REQUIRE(done_with == "x/closed");
    REQUIRE(!f.menu.isOpen());
    REQUIRE(f.menu.entry() == nullptr);
}

TEST_CASE("A position entry is set like a clock and typed like a VCR", "[Menu]") {
    double done = -1;
    int cancelled = 0;
    PositionEntry time("GO TO TIME", PositionEntry::Format::eTime, 0, 3091, [&](double v) { done = v; }, [&] { cancelled++; });
    CHECK(time.text() == "0:51:31");
    CHECK(time.cursor() == 6); // the units of the seconds, past both colons
    time.handleKey(Entry::Key::eUp);
    CHECK(time.text() == "0:51:32");
    time.handleKey(Entry::Key::eLeft);
    CHECK(time.cursor() == 5);
    for (int i = 0; i < 3; i++) time.handleKey(Entry::Key::eUp); // 3 -> 4 -> 5 -> 0: the tens of seconds stop at 5
    CHECK(time.text() == "0:51:02");
    time.handleKey(Entry::Key::eDown);
    CHECK(time.text() == "0:51:52");
    time.handleKey(Entry::Key::eLeft); // the units of the minutes
    time.handleKey(Entry::Key::eLeft); // the tens
    time.handleKey(Entry::Key::eLeft); // the hours
    CHECK(time.cursor() == 0);
    time.handleKey(Entry::Key::eLeft); // wraps to the seconds
    CHECK(time.cursor() == 6);
    time.handleKey(Entry::Key::eRight);
    CHECK(time.cursor() == 0);
    // A keyboard types the digits in order, overwriting from the cursor
    for (const char *d : {"1", "0", "2", "3", "4"}) time.type(d);
    CHECK(time.text() == "1:02:34");
    CHECK(time.cursor() == 6); // stays on the last digit
    time.type("9");
    CHECK(time.text() == "1:02:39");
    time.type("x"); // not a digit
    CHECK(time.text() == "1:02:39");
    time.backspace();
    CHECK(time.text() == "1:02:30");
    CHECK(time.cursor() == 5);
    time.handleKey(Entry::Key::eSelect);
    CHECK(done == 3600 + 2 * 60 + 30);
    time.handleKey(Entry::Key::eBack);
    CHECK(cancelled == 1);

    PositionEntry frame("GO TO FRAME", PositionEntry::Format::eNumber, 5, 23456, [&](double v) { done = v; }, {});
    CHECK(frame.text() == "23456");
    CHECK(frame.cursor() == 4);
    frame.handleKey(Entry::Key::eUp);
    CHECK(frame.text() == "23457");
    for (int i = 0; i < 4; i++) frame.handleKey(Entry::Key::eUp); // 7, 8, 9, 0, 1: no carry
    CHECK(frame.text() == "23451");
    frame.accept();
    CHECK(done == 23451);
    PositionEntry chapter("GO TO CHAPTER", PositionEntry::Format::eNumber, 2, 7, {}, {});
    CHECK(chapter.text() == "07");
    PositionEntry clamped("X", PositionEntry::Format::eNumber, 2, 123, {}, {});
    CHECK(clamped.text() == "99");
    CHECK(clamped.hint().find("SELECT") != std::string::npos);
    CHECK(clamped.rows().empty());
}

TEST_CASE("An entry started on a closed menu opens it for its own duration", "[Menu]") {
    Fixture f;
    REQUIRE(!f.menu.isOpen());
    f.menu.startEntry(std::make_unique<PositionEntry>("GO TO", PositionEntry::Format::eNumber, 2, 3,
                                                      [&](double) { f.menu.close(); }, [&] { f.menu.endEntry(); }));
    CHECK(f.menu.isOpen());
    REQUIRE(f.menu.entry() != nullptr);
    f.menu.handleKey(MenuKey::eBack); // cancels: the entry ends, and the menu it opened closes
    CHECK(f.menu.entry() == nullptr);
    CHECK(!f.menu.isOpen());

    f.menu.startEntry(std::make_unique<PositionEntry>("GO TO", PositionEntry::Format::eNumber, 2, 3,
                                                      [&](double) { f.menu.close(); }, [&] { f.menu.endEntry(); }));
    f.menu.handleKey(MenuKey::eSelect); // accepts: the callback closes the menu itself
    CHECK(f.menu.entry() == nullptr);
    CHECK(!f.menu.isOpen());

    // From an open menu the entry's end returns to the page it was started from
    f.menu.open();
    f.menu.startEntry(std::make_unique<PositionEntry>("GO TO", PositionEntry::Format::eNumber, 2, 3,
                                                      [&](double) {}, [&] { f.menu.endEntry(); }));
    f.menu.handleKey(MenuKey::eBack);
    CHECK(f.menu.entry() == nullptr);
    CHECK(f.menu.isOpen());
}
