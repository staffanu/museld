// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <pthread.h>
#include "input/HttpClient.h" // httplib.h with its configuration
#include "HttpByteSource.h"
#include "logging/Logger.h"
#include "util/ThreadRole.h"

namespace {

// Chunks this size are fetched this many ahead of the reader, on this many
// connections.  A range request has a start-up cost the server pays before the
// first byte moves, and on a file that is not in its page cache it sits in
// series with the transfer on one connection; measured in 2026-11 on an Apple TV
// reading cold files from a FreeBSD/Apache server on gigabit: 4 MiB chunks on
// one connection 79 MB/s from an SSD and 42 from a spinning disk, 16 MiB chunks
// 99 and 72-88, a second connection 119 from the SSD but 15-24 from the disk,
// whose head then seeks between the two streams.  So: 16 MiB chunks, one
// connection, two ahead (32 MiB in flight, half a second of a 62.5 MHz 8-bit
// capture, which rides out a NAS that hesitates).  The chunks are claimed in
// file order and queued in file order whichever connection finishes first.
// MUSELD_HTTP_CONNECTIONS and MUSELD_HTTP_CHUNK_MIB override the two for A/B.
constexpr size_t c_default_chunk_bytes = 16u << 20;
constexpr int c_default_connections = 1;
constexpr int c_chunks_ahead = 2; // completed or in flight, beyond the one being read
constexpr int c_fetch_attempts = 4;

int envInt(const char *name, int fallback, int lo, int hi) {
    const char *v = getenv(name);
    if (v == nullptr || *v == 0)
        return fallback;
    return std::clamp(atoi(v), lo, hi);
}

struct ParsedUrl {
    std::string origin; // scheme://host[:port], what httplib::Client takes
    std::string path;   // /path?query, what goes in the request line
};

ParsedUrl parseUrl(const std::string &url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos)
        throw std::runtime_error(std::format("Not a URL: {}", url));
    const auto path_start = url.find('/', scheme_end + 3);
    ParsedUrl parsed;
    parsed.origin = url.substr(0, path_start);
    parsed.path = path_start == std::string::npos ? "/" : url.substr(path_start);
    if (parsed.origin.size() == scheme_end + 3)
        throw std::runtime_error(std::format("No host in URL: {}", url));
    return parsed;
}

std::unique_ptr<httplib::Client> makeClient(const ParsedUrl &url) {
    auto client = std::make_unique<httplib::Client>(url.origin);
    client->set_keep_alive(true);
    // A large socket receive buffer, beside the large receive pieces of
    // input/HttpClient.h (the pieces are what measured; this is kept as cheap
    // and plausible, its own effect being within the run-to-run variance of
    // the NAS)
    client->set_socket_options([](socket_t sock) {
        const int bytes = 4 << 20;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&bytes), sizeof bytes);
    });
    client->set_connection_timeout(std::chrono::seconds(5));
    client->set_read_timeout(std::chrono::seconds(15));
    client->set_write_timeout(std::chrono::seconds(5));
    return client;
}

std::string describe(const httplib::Result &result) {
    return result ? std::format("HTTP status {}", result->status) : httplib::to_string(result.error());
}

// The total after the slash in "bytes 0-0/12345", or -1.
int64_t totalFromContentRange(const std::string &header) {
    const auto slash = header.rfind('/');
    if (slash == std::string::npos || slash + 1 >= header.size() || header[slash + 1] == '*')
        return -1;
    try {
        return std::stoll(header.substr(slash + 1));
    } catch (const std::exception &) {
        return -1;
    }
}

// Asks for the size with a one-byte range request: HEAD is optional for a
// server, but a range response always carries the total, and it also proves
// that ranges are honoured before the first real fetch.
int64_t querySize(httplib::Client &client, const std::string &path, bool &ranges_ok, std::string &error) {
    auto result = client.Get(path, httplib::Headers{{"Range", "bytes=0-0"}});
    if (!result) {
        error = describe(result);
        return -1;
    }
    if (result->status == 206) {
        ranges_ok = true;
        return totalFromContentRange(result->get_header_value("Content-Range"));
    }
    if (result->status == 200) {
        // The whole file came back: the server ignores Range headers.
        ranges_ok = false;
        error = "the server does not support range requests (it answered a range request with the whole file)";
        return -1;
    }
    error = describe(result);
    return -1;
}

} // namespace

struct HttpByteSource::Impl {
    struct Chunk {
        int64_t offset;
        std::string data;
        int64_t end() const { return offset + (int64_t)data.size(); }
    };

    Impl(const std::string &url, Logger *log)
        : m_url(parseUrl(url)), m_log(log),
          m_chunk_bytes((size_t)envInt("MUSELD_HTTP_CHUNK_MIB", (int)(c_default_chunk_bytes >> 20), 1, 64) << 20),
          m_connections(envInt("MUSELD_HTTP_CONNECTIONS", c_default_connections, 1, 8)) {
        auto client = makeClient(m_url);
        bool ranges_ok = false;
        std::string error;
        m_size = querySize(*client, m_url.path, ranges_ok, error);
        if (!ranges_ok)
            throw std::runtime_error(std::format("Cannot read {}: {}", url, error));
        if (m_log)
            m_log->info(eInput, std::format("{}: {} bytes on {} (received in pieces of {} KiB, {} MiB chunks, {} ahead, on {} connection{})",
                                            m_url.path, m_size >= 0 ? std::to_string(m_size) : "unknown", m_url.origin,
                                            CPPHTTPLIB_RECV_BUFSIZ / 1024, m_chunk_bytes >> 20, c_chunks_ahead,
                                            m_connections, m_connections == 1 ? "" : "s"));
        m_fetchers.emplace_back([this, c = std::move(client)]() mutable { produce(*c); });
        for (int i = 1; i < m_connections; i++)
            m_fetchers.emplace_back([this]() { auto c = makeClient(m_url); produce(*c); });
    }

    ~Impl() {
        {
            std::scoped_lock lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        for (auto &fetcher : m_fetchers)
            if (fetcher.joinable())
                fetcher.join();
    }

    // One range request, retried on transient failures.  Returns the bytes,
    // fewer than asked for only at the end of the file.
    std::string fetch(httplib::Client &client, int64_t offset, size_t length) {
        std::string last_error;
        for (int attempt = 0; attempt < c_fetch_attempts; attempt++) {
            if (attempt > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(200 * attempt));
            auto result = client.Get(m_url.path, httplib::Headers{{"Range", std::format("bytes={}-{}", offset, offset + (int64_t)length - 1)}});
            if (result && result->status == 206)
                return std::move(result->body);
            if (result && result->status == 416)
                return {}; // past the end
            last_error = describe(result);
            if (m_log)
                m_log->warn(eInput, std::format("HTTP fetch of bytes {}+{} failed ({}), attempt {} of {}",
                                                offset, length, last_error, attempt + 1, c_fetch_attempts));
        }
        throw std::runtime_error(std::format("HTTP read of {} failed: {}", m_url.path, last_error));
    }

    // A fetcher: claims the next chunk in file order, fetches it on its own
    // connection, and files it under its offset.  The queue holds at most
    // c_chunks_ahead chunks completed or in flight beyond the one being read.
    void produce(httplib::Client &client) {
        setThreadRole(ThreadRole::eInput, "input-http");
        std::unique_lock lock(m_mutex);
        while (!m_stop) {
            const bool at_end = m_end_at >= 0 && m_fetch_pos >= m_end_at;
            if ((int)m_chunks.size() + m_in_flight >= c_chunks_ahead + 1 || at_end || m_error) {
                m_cv.wait(lock);
                continue;
            }
            const int64_t offset = m_fetch_pos;
            const uint64_t generation = m_generation;
            m_fetch_pos += (int64_t)m_chunk_bytes;
            m_in_flight++;
            lock.unlock();
            std::string data;
            std::exception_ptr error;
            try {
                data = fetch(client, offset, m_chunk_bytes);
            } catch (...) {
                error = std::current_exception();
            }
            lock.lock();
            m_in_flight--;
            if (generation != m_generation) {
                m_cv.notify_all();
                continue; // a seek happened meanwhile; this chunk is for the old position
            }
            if (error) {
                m_error = error;
                m_cv.notify_all();
                return;
            }
            if (data.size() < m_chunk_bytes) {
                // The end of the file: nothing claimed beyond it is worth keeping
                const int64_t end = offset + (int64_t)data.size();
                if (m_end_at < 0 || end < m_end_at)
                    m_end_at = end;
                for (auto it = m_chunks.begin(); it != m_chunks.end();)
                    it = it->first >= m_end_at ? m_chunks.erase(it) : std::next(it);
            }
            if (!data.empty() && (m_end_at < 0 || offset < m_end_at))
                m_chunks.emplace(offset, Chunk{offset, std::move(data)});
            m_cv.notify_all();
        }
    }

    // The chunk holding the reader's position, once it has arrived
    std::map<int64_t, Chunk>::iterator currentChunk() {
        auto it = m_chunks.upper_bound(m_pos);
        if (it == m_chunks.begin())
            return m_chunks.end();
        --it;
        return m_pos < it->second.end() ? it : m_chunks.end();
    }

    ssize_t read(uint8_t *buf, size_t len) {
        std::unique_lock lock(m_mutex);
        std::map<int64_t, Chunk>::iterator current;
        for (;;) {
            if (m_error)
                std::rethrow_exception(m_error);
            // Chunks the reader has passed (a chunk arriving after a seek within it)
            while (!m_chunks.empty() && m_chunks.begin()->second.end() <= m_pos)
                m_chunks.erase(m_chunks.begin());
            current = currentChunk();
            if (current != m_chunks.end())
                break;
            if (m_end_at >= 0 && m_pos >= m_end_at)
                return 0;
            // The reader is ahead of the network: the time it waits here is what the
            // source costs the pipeline (--log P3, every 256 MiB)
            const auto t_wait = std::chrono::steady_clock::now();
            m_cv.wait(lock);
            m_stats_wait_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_wait).count();
        }
        if (m_log) {
            if (m_stats_bytes == 0)
                m_stats_start = std::chrono::steady_clock::now();
            m_stats_bytes += (int64_t)len;
            if (m_stats_bytes >= (int64_t)256 << 20) {
                const double elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_stats_start).count();
                m_log->info(ePerformance, std::format("http input: {:.1f} MB/s consumed, the reader waited for the network {:.0f}% of the time",
                                                      m_stats_bytes / 1e6 / elapsed_s, 100.0 * m_stats_wait_us / 1e6 / elapsed_s));
                m_stats_bytes = 0;
                m_stats_wait_us = 0;
            }
        }
        Chunk &chunk = current->second;
        const int64_t within = m_pos - chunk.offset;
        const size_t n = std::min(len, (size_t)(chunk.end() - m_pos));
        memcpy(buf, chunk.data.data() + within, n);
        m_pos += (int64_t)n;
        if (m_pos == chunk.end()) {
            m_chunks.erase(current);
            m_cv.notify_all();
        }
        return (ssize_t)n;
    }

    int64_t seek(int64_t offset, int whence) {
        std::scoped_lock lock(m_mutex);
        int64_t target;
        switch (whence) {
            case SEEK_SET: target = offset; break;
            case SEEK_CUR: target = m_pos + offset; break;
            case SEEK_END:
                if (m_size < 0) {
                    errno = EINVAL;
                    return -1;
                }
                target = m_size + offset;
                break;
            default:
                errno = EINVAL;
                return -1;
        }
        target = std::max<int64_t>(0, target);
        // Within what is queued (contiguous from the chunk being read): just move
        if (!m_chunks.empty() && target >= m_chunks.begin()->first && target < m_fetch_pos
            && (m_end_at < 0 || target < m_end_at)) {
            m_pos = target;
            return m_pos;
        }
        m_chunks.clear();
        m_pos = m_fetch_pos = target;
        m_end_at = -1;
        m_generation++;
        m_cv.notify_all();
        return m_pos;
    }

    ParsedUrl m_url;
    Logger *m_log;
    const size_t m_chunk_bytes;
    const int m_connections;
    int64_t m_size = -1;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::map<int64_t, Chunk> m_chunks; // fetched, by offset; contiguous from the reader's chunk to m_fetch_pos less what is in flight
    int m_in_flight = 0;        // chunks claimed and being fetched
    int64_t m_pos = 0;          // the reader's position
    int64_t m_fetch_pos = 0;    // where the next chunk to claim starts
    int64_t m_end_at = -1;      // the end of the file once a short chunk revealed it
    uint64_t m_generation = 0;  // bumped by seeks, so a fetch in flight is discarded
    std::exception_ptr m_error;
    int64_t m_stats_bytes = 0, m_stats_wait_us = 0; // the P3 statistics, see read()
    std::chrono::steady_clock::time_point m_stats_start;
    bool m_stop = false;
    std::vector<std::thread> m_fetchers; // last: joined before the state above goes away
};

HttpByteSource::HttpByteSource(const std::string &url, Logger *log) : m_impl(std::make_unique<Impl>(url, log)) {}

HttpByteSource::~HttpByteSource() = default;

ssize_t HttpByteSource::read(uint8_t *buf, size_t len) {
    return m_impl->read(buf, len);
}

int64_t HttpByteSource::seek(int64_t offset, int whence) {
    return m_impl->seek(offset, whence);
}

int64_t HttpByteSource::size() {
    return m_impl->m_size;
}

int64_t HttpByteSource::sizeOf(const std::string &url) {
    try {
        const ParsedUrl parsed = parseUrl(url);
        auto client = makeClient(parsed);
        bool ranges_ok = false;
        std::string error;
        return querySize(*client, parsed.path, ranges_ok, error);
    } catch (const std::exception &) {
        return -1;
    }
}
