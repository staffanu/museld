// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

// HttpByteSource against an in-process web server: the bytes come out in file
// order whichever connection fetched them, seeks land where they say, and the
// end of the file is the end.

#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>
#include "input/HttpClient.h"
#include "input/HttpByteSource.h"

namespace {

// A file of pseudo-random bytes, served from a temporary directory by httplib
// (which answers Range requests on mounted files with 206)
struct Served {
    std::filesystem::path dir;
    std::string bytes;
    httplib::Server server;
    int port = 0;
    std::thread thread;
    std::atomic<int> requests{0};

    explicit Served(size_t size) {
        dir = std::filesystem::temp_directory_path() / std::format("httpbytesource-test-{}", getpid());
        std::filesystem::create_directories(dir);
        bytes.resize(size);
        std::mt19937 rng(12345);
        for (auto &b : bytes) b = (char)rng();
        std::ofstream(dir / "capture.s8", std::ios::binary).write(bytes.data(), (std::streamsize)bytes.size());
        server.set_mount_point("/", dir.string());
        server.set_logger([this](const httplib::Request &, const httplib::Response &) { requests++; });
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
    }
    ~Served() {
        server.stop();
        thread.join();
        std::filesystem::remove_all(dir);
    }
    std::string url() const { return std::format("http://127.0.0.1:{}/capture.s8", port); }
};

std::string readAll(ByteSource &source, size_t piece) {
    std::string out;
    std::string buf(piece, 0);
    for (;;) {
        ssize_t n = source.read((uint8_t *)buf.data(), piece);
        REQUIRE(n >= 0);
        if (n == 0) break;
        out.append(buf.data(), (size_t)n);
    }
    return out;
}

struct Env {
    Env(const char *name, const char *value) : m_name(name) { set(name, value); }
    ~Env() { set(m_name, ""); } // MinGW has no unsetenv; an empty value is what the code treats as unset
    static void set(const char *name, const char *value) {
#ifdef _WIN32
        _putenv_s(name, value);
#else
        if (*value) setenv(name, value, 1); else unsetenv(name);
#endif
    }
    const char *m_name;
};

} // namespace

TEST_CASE("The whole file comes out in order, on one and on several connections", "[HttpByteSource]") {
    // Small chunks so that a 10 MiB file spans many of them and the end is a partial one
    Env chunk("MUSELD_HTTP_CHUNK_MIB", "1");
    Served served(10 * 1024 * 1024 + 12345);
    for (const char *connections : {"1", "2", "3"}) {
        Env conn("MUSELD_HTTP_CONNECTIONS", connections);
        HttpByteSource source(served.url(), nullptr);
        REQUIRE(source.size() == (int64_t)served.bytes.size());
        REQUIRE(readAll(source, 300007) == served.bytes);
    }
}

TEST_CASE("Seeks land where they say, forwards, backwards and within the queue", "[HttpByteSource]") {
    Env chunk("MUSELD_HTTP_CHUNK_MIB", "1");
    Env conn("MUSELD_HTTP_CONNECTIONS", "2");
    Served served(6 * 1024 * 1024 + 777);
    HttpByteSource source(served.url(), nullptr);
    auto check = [&](int64_t at, size_t len) {
        std::string buf(len, 0);
        size_t got = 0;
        while (got < len) {
            ssize_t n = source.read((uint8_t *)buf.data() + got, len - got);
            REQUIRE(n > 0);
            got += (size_t)n;
        }
        REQUIRE(buf == served.bytes.substr((size_t)at, len));
    };
    check(0, 1000);
    REQUIRE(source.seek(5 * 1024 * 1024 + 3, SEEK_SET) == 5 * 1024 * 1024 + 3); // far forward, past the queue
    check(5 * 1024 * 1024 + 3, 70000);
    REQUIRE(source.seek(-2 * 1024 * 1024, SEEK_CUR) == 3 * 1024 * 1024 + 70003); // backwards
    check(3 * 1024 * 1024 + 70003, 2 * 1024 * 1024); // across chunk boundaries
    REQUIRE(source.seek(100, SEEK_CUR) == 5 * 1024 * 1024 + 70103); // a short hop, within what is queued
    check(5 * 1024 * 1024 + 70103, 50000);
    REQUIRE(source.seek(-10, SEEK_END) == (int64_t)served.bytes.size() - 10);
    REQUIRE(readAll(source, 4096) == served.bytes.substr(served.bytes.size() - 10));
    REQUIRE(source.seek(-1000000000, SEEK_SET) == 0); // clamped at the start
    check(0, 10);
}

TEST_CASE("A file of exactly whole chunks ends cleanly", "[HttpByteSource]") {
    Env chunk("MUSELD_HTTP_CHUNK_MIB", "1");
    Env conn("MUSELD_HTTP_CONNECTIONS", "2");
    Served served(3 * 1024 * 1024);
    HttpByteSource source(served.url(), nullptr);
    REQUIRE(readAll(source, 65536) == served.bytes);
    uint8_t b;
    REQUIRE(source.read(&b, 1) == 0); // the end persists
}
