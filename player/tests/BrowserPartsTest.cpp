// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

// The browser's parts without a window: the index page parser and the
// location arithmetic, the text entry's keyboard, the saved state's file.

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <unistd.h>
#include "ui/DirectoryListing.h"
#include "ui/Menu.h"
#include "ui/SavedState.h"
#include "ui/TextEntry.h"

TEST_CASE("An Apache index page lists its files with sizes, directories first, without the parent", "[Browser]") {
    const std::string html = R"(<html><head><title>Index of /ld-local/MUSE</title></head><body>
<h1>Index of /ld-local/MUSE</h1>
  <table>
   <tr><th valign="top">&nbsp;</th><th><a href="?C=N;O=D">Name</a></th><th><a href="?C=M;O=A">Last modified</a></th><th><a href="?C=S;O=A">Size</a></th></tr>
   <tr><th colspan="5"><hr></th></tr>
<tr><td valign="top">&nbsp;</td><td><a href="/ld-local/">Parent Directory</a></td><td>&nbsp;</td><td align="right">  - </td><td>&nbsp;</td></tr>
<tr><td valign="top">&nbsp;</td><td><a href="zeta%20disc.s8">zeta disc.s8</a></td><td align="right">2026-10-22 01:49  </td><td align="right">2.2G</td><td>&nbsp;</td></tr>
<tr><td valign="top">&nbsp;</td><td><a href="UrsaMinorBlue/">UrsaMinorBlue/</a></td><td align="right">2026-10-22 01:51  </td><td align="right">  - </td><td>&nbsp;</td></tr>
<tr><td valign="top">&nbsp;</td><td><a href="alpha.ldf">alpha.ldf</a></td><td align="right">2026-10-22 01:50  </td><td align="right">71G</td><td>&nbsp;</td></tr>
<tr><td valign="top">&nbsp;</td><td><a href="http://elsewhere/x.s8">external</a></td><td>&nbsp;</td><td align="right">  - </td><td>&nbsp;</td></tr>
   <tr><th colspan="5"><hr></th></tr>
</table></body></html>)";
    const auto entries = DirectoryListing::parseHtmlIndex(html);
    REQUIRE(entries.size() == 3);
    REQUIRE(entries[0].name == "UrsaMinorBlue");
    REQUIRE(entries[0].is_dir);
    REQUIRE(entries[0].href == "UrsaMinorBlue/");
    REQUIRE(entries[1].name == "alpha.ldf");
    REQUIRE(!entries[1].is_dir);
    REQUIRE(entries[1].detail == "71G");
    REQUIRE(entries[2].name == "zeta disc.s8");
    REQUIRE(entries[2].href == "zeta%20disc.s8");
    REQUIRE(entries[2].detail == "2.2G");
}

TEST_CASE("A plain list of links (Python's http.server) parses too", "[Browser]") {
    const std::string html = "<ul>\n<li><a href=\"b.s16\">b.s16</a></li>\n<li><a href=\"a/\">a/</a></li>\n<li><a href=\"../\">../</a></li></ul>";
    const auto entries = DirectoryListing::parseHtmlIndex(html);
    REQUIRE(entries.size() == 2);
    REQUIRE(entries[0].name == "a");
    REQUIRE(entries[0].is_dir);
    REQUIRE(entries[1].name == "b.s16");
    REQUIRE(entries[1].detail.empty());
}

TEST_CASE("Locations: parents, joins and slashes, for paths and URLs", "[Browser]") {
    using namespace DirectoryListing;
    REQUIRE(parent("http://nas/ld-local/MUSE/") == "http://nas/ld-local/");
    REQUIRE(parent("http://nas/ld-local/") == "http://nas/");
    REQUIRE(parent("http://nas/") == "http://nas/");
    REQUIRE(parent("http://nas") == "http://nas/");
    REQUIRE(parent("/a/b/") == "/a/");
    REQUIRE(parent("/a/") == "/");
    REQUIRE(parent("/") == "/");
    DirEntry dir{"x", "x/", true, ""};
    REQUIRE(join("http://nas/ld-local", dir) == "http://nas/ld-local/x/");
    REQUIRE(join("/a/", dir) == "/a/x/");
    REQUIRE(withSlash("/a") == "/a/");
    REQUIRE(withSlash("/a/") == "/a/");
    REQUIRE(isHttp("http://x"));
    REQUIRE(!isHttp("udp://x:5000"));
    REQUIRE(isUrl("udp://x:5000"));
}

TEST_CASE("The text entry types from its keyboard and from keys, and ends by DONE or CANCEL", "[Browser]") {
    std::string done;
    int cancelled = 0;
    TextEntry entry("ADDRESS", "http://", [&](const std::string &t) { done = t; }, [&] { cancelled++; });
    REQUIRE(entry.text() == "http://");
    entry.type("nas");
    REQUIRE(entry.text() == "http://nas");
    entry.backspace();
    REQUIRE(entry.text() == "http://na");
    // the keyboard: row 1 is q w e r t y u i o p . :  -- type an 's' from row 2
    entry.handleKey(TextEntry::Key::eDown);
    entry.handleKey(TextEntry::Key::eDown);
    entry.handleKey(TextEntry::Key::eRight);
    entry.handleKey(TextEntry::Key::eSelect);
    REQUIRE(entry.text() == "http://nas");
    // SHIFT then a letter gives one capital
    entry.handleKey(TextEntry::Key::eDown);
    entry.handleKey(TextEntry::Key::eDown); // the word row, where the column clamps to SPACE
    entry.handleKey(TextEntry::Key::eLeft); // SHIFT
    entry.handleKey(TextEntry::Key::eLeft); // wraps to DONE
    REQUIRE(entry.rows()[entry.row()][entry.column()] == "DONE");
    entry.handleKey(TextEntry::Key::eSelect);
    REQUIRE(done == "http://nas");
    // Back deletes, and cancels once the text is empty
    TextEntry e2("A", "ab", [](const std::string &) {}, [&] { cancelled++; });
    e2.handleKey(TextEntry::Key::eBack);
    e2.handleKey(TextEntry::Key::eBack);
    REQUIRE(e2.text().empty());
    REQUIRE(cancelled == 0);
    e2.handleKey(TextEntry::Key::eBack);
    REQUIRE(cancelled == 1);
}

TEST_CASE("The saved state keeps the recent addresses and the last directory across a reload", "[Browser]") {
    const auto path = (std::filesystem::temp_directory_path() / std::format("museld-state-test-{}.txt", ::getpid())).string();
    std::filesystem::remove(path);
    {
        SavedState state(path);
        REQUIRE(state.recent().empty());
        state.addRecent("http://nas/a.s8");
        state.addRecent("udp://cap:5000");
        state.addRecent("http://nas/a.s8"); // moves to the front, once
        state.setLastDirectory("http://nas/");
    }
    {
        SavedState state(path);
        REQUIRE(state.recent().size() == 2);
        REQUIRE(state.recent()[0] == "http://nas/a.s8");
        REQUIRE(state.recent()[1] == "udp://cap:5000");
        REQUIRE(state.lastDirectory() == "http://nas/");
        state.removeRecent("udp://cap:5000");
    }
    {
        SavedState state(path);
        REQUIRE(state.recent().size() == 1);
    }
    std::filesystem::remove(path);
}

TEST_CASE("Menu: an item that replaces its own page's items when activated", "[menu]") {
    // The browser's items rebuild the pages they are on: the item running is
    // destroyed, so the menu must not touch it after calling
    Menu menu;
    MenuPage *page = menu.addPage("P");
    std::string chosen;
    std::function<void()> rebuild;
    rebuild = [&] {
        std::vector<MenuItem> items;
        for (int i = 0; i < 3; i++) {
            MenuItem item;
            item.label = "item " + std::to_string(i);
            item.activate = [&, name = item.label] {
                rebuild();
                chosen = name;
            };
            items.push_back(std::move(item));
        }
        menu.setItems(page, std::move(items));
    };
    rebuild();
    menu.open();
    menu.handleKey(MenuKey::eDown);
    menu.handleKey(MenuKey::eSelect);
    CHECK(chosen == "item 1");
}
