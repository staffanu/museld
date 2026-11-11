// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

// The subtitle files of an input: the name matching, a directory, and a web
// server's index page with the files fetched over HTTP.

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>
#include <unistd.h>
#include "input/HttpClient.h"
#include "subtitles/SubtitleDiscovery.h"

namespace {

const char *c_srt = "1\n00:00:01,000 --> 00:00:02,000\nhello\n";

struct TempDir {
    std::filesystem::path dir;
    explicit TempDir(const char *tag) {
        dir = std::filesystem::temp_directory_path() / std::format("subtitle-discovery-{}-{}", tag, getpid());
        std::filesystem::create_directories(dir);
    }
    ~TempDir() { std::filesystem::remove_all(dir); }
    void put(const std::string &name, const std::string &text = c_srt) const {
        std::ofstream(dir / name, std::ios::binary) << text;
    }
};

} // namespace

TEST_CASE("Subtitle names: the input's stem, the label after it, sorted", "[Subtitles]") {
    const auto found = matchSubtitleNames("blue 1.ldf", {
        "blue 1.ldf", "blue 1.ja.srt", "blue 1.srt", "blue 1.en-US.srt", "blue 2.srt", "other.srt", ".srt", "blue 1.txt"});
    REQUIRE(found.size() == 3);
    REQUIRE(found[0] == std::pair<std::string, std::string>{"en-US", "blue 1.en-US.srt"});
    REQUIRE(found[1] == std::pair<std::string, std::string>{"ja", "blue 1.ja.srt"});
    REQUIRE(found[2] == std::pair<std::string, std::string>{"DEFAULT", "blue 1.srt"});
    // An input without an extension is its own stem
    REQUIRE(matchSubtitleNames("capture", {"capture.srt"}).size() == 1);
}

TEST_CASE("Subtitle files are found in the input's directory", "[Subtitles]") {
    TempDir temp("dir");
    temp.put("capture.s8", "x");
    temp.put("capture.ja.srt");
    temp.put("unrelated.srt");
    const auto found = discoverSubtitleFiles((temp.dir / "capture.s8").string());
    REQUIRE(found.size() == 1);
    REQUIRE(found[0].label == "ja");
    REQUIRE(loadSubtitleFile(found[0].location).size() == 1);
}

TEST_CASE("Subtitle files are found on a web server's index page and fetched from it", "[Subtitles]") {
    TempDir temp("http");
    temp.put("blue 1.srt");
    temp.put("blue 1.ja.srt", "1\n00:00:01,000 --> 00:00:02,000\nhello\n\n2\n00:00:03,000 --> 00:00:04,000\nagain\n");
    httplib::Server server;
    server.set_mount_point("/files", temp.dir.string());
    // An index page as Apache writes its links: percent-encoded, relative
    server.Get("/ld/", [](const httplib::Request &, httplib::Response &response) {
        response.set_content("<html><body><a href=\"/\">Parent Directory</a>"
                             "<a href=\"blue%201.ldf\">blue 1.ldf</a>"
                             "<a href=\"blue%201.srt\">blue 1.srt</a>"
                             "<a href=\"blue%201.ja.srt\">blue 1.ja.srt</a>"
                             "<a href=\"blue%202.srt\">blue 2.srt</a>"
                             "<a href=\"sub/\">sub/</a></body></html>", "text/html");
    });
    server.Get(R"(/ld/(.+\.srt))", [&](const httplib::Request &request, httplib::Response &response) {
        std::ifstream in(temp.dir / request.matches[1].str(), std::ios::binary);
        if (!in) { response.status = 404; return; }
        response.set_content(std::string(std::istreambuf_iterator<char>(in), {}), "text/plain");
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    std::thread thread([&] { server.listen_after_bind(); });
    server.wait_until_ready();
    const std::string base = std::format("http://127.0.0.1:{}", port);

    const auto found = discoverSubtitleFiles(base + "/ld/blue%201.ldf");
    const auto none = discoverSubtitleFiles(base + "/nowhere/blue%201.ldf");
    std::vector<SubtitleEntry> entries;
    std::string missing;
    if (found.size() == 2)
        entries = loadSubtitleFile(found[0].location);
    try { loadSubtitleFile(base + "/ld/absent.srt"); } catch (const std::runtime_error &x) { missing = x.what(); }
    server.stop();
    thread.join();

    REQUIRE(found.size() == 2);
    REQUIRE(found[0] == SubtitleFile{"ja", base + "/ld/blue%201.ja.srt"});
    REQUIRE(found[1] == SubtitleFile{"DEFAULT", base + "/ld/blue%201.srt"});
    REQUIRE(entries.size() == 2);
    REQUIRE(none.empty());
    REQUIRE(missing.find("404") != std::string::npos);
    // Not a place to look
    REQUIRE(discoverSubtitleFiles("udp://:5000").empty());
    REQUIRE(discoverSubtitleFiles(base).empty());
}
