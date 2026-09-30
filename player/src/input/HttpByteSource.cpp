// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <pthread.h>
#include "httplib.h"
#include "HttpByteSource.h"
#include "logging/Logger.h"

namespace {

// Chunks this size are fetched this many ahead of the reader: 12 MiB in
// flight covers a 62.5 MHz 8-bit capture for about 190 ms, which rides out a
// NAS that hesitates without the decoder noticing.
constexpr size_t c_chunk_bytes = 4u << 20;
constexpr int c_chunks_ahead = 3;
constexpr int c_fetch_attempts = 4;

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

    Impl(const std::string &url, Logger *log) : m_url(parseUrl(url)), m_log(log), m_client(makeClient(m_url)) {
        bool ranges_ok = false;
        std::string error;
        m_size = querySize(*m_client, m_url.path, ranges_ok, error);
        if (!ranges_ok)
            throw std::runtime_error(std::format("Cannot read {}: {}", url, error));
        if (m_log)
            m_log->info(eInput, std::format("{}: {} bytes on {}", m_url.path,
                                            m_size >= 0 ? std::to_string(m_size) : "unknown", m_url.origin));
        m_producer = std::thread([this] { produce(); });
    }

    ~Impl() {
        {
            std::scoped_lock lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        if (m_producer.joinable())
            m_producer.join();
    }

    // One range request, retried on transient failures.  Returns the bytes,
    // fewer than asked for only at the end of the file.
    std::string fetch(int64_t offset, size_t length) {
        std::string last_error;
        for (int attempt = 0; attempt < c_fetch_attempts; attempt++) {
            if (attempt > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(200 * attempt));
            auto result = m_client->Get(m_url.path, httplib::Headers{{"Range", std::format("bytes={}-{}", offset, offset + (int64_t)length - 1)}});
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

    void produce() {
#ifdef __APPLE__
        pthread_setname_np("input-http");
#elif defined(linux) || defined(__FreeBSD__)
        pthread_setname_np(pthread_self(), "input-http");
#endif
        std::unique_lock lock(m_mutex);
        while (!m_stop) {
            const bool at_end = m_end_at >= 0 && m_fetch_pos >= m_end_at;
            if ((int)m_chunks.size() >= c_chunks_ahead || at_end) {
                m_cv.wait(lock);
                continue;
            }
            const int64_t offset = m_fetch_pos;
            const uint64_t generation = m_generation;
            lock.unlock();
            std::string data;
            try {
                data = fetch(offset, c_chunk_bytes);
            } catch (...) {
                lock.lock();
                m_error = std::current_exception();
                m_cv.notify_all();
                return;
            }
            lock.lock();
            if (generation != m_generation)
                continue; // a seek happened meanwhile; this chunk is for the old position
            m_chunks.push_back({offset, std::move(data)});
            m_fetch_pos += (int64_t)m_chunks.back().data.size();
            if (m_chunks.back().data.size() < c_chunk_bytes)
                m_end_at = m_fetch_pos;
            m_cv.notify_all();
        }
    }

    ssize_t read(uint8_t *buf, size_t len) {
        std::unique_lock lock(m_mutex);
        for (;;) {
            if (m_error)
                std::rethrow_exception(m_error);
            // Empty chunks mark the end and carry nothing; drop them.
            while (!m_chunks.empty() && m_chunks.front().data.empty())
                m_chunks.pop_front();
            if (!m_chunks.empty())
                break;
            if (m_end_at >= 0 && m_pos >= m_end_at)
                return 0;
            m_cv.wait(lock);
        }
        Chunk &front = m_chunks.front();
        // The consumer's position always lies inside the front chunk: seeks
        // that leave it discard the queue.
        const int64_t within = m_pos - front.offset;
        const size_t n = std::min(len, (size_t)(front.end() - m_pos));
        memcpy(buf, front.data.data() + within, n);
        m_pos += (int64_t)n;
        if (m_pos == front.end()) {
            m_chunks.pop_front();
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
        if (!m_chunks.empty() && target >= m_chunks.front().offset && target < m_chunks.front().end()) {
            m_pos = target; // still inside the chunk being read
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
    std::unique_ptr<httplib::Client> m_client;
    int64_t m_size = -1;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Chunk> m_chunks; // fetched, in order, the front one being consumed
    int64_t m_pos = 0;          // the reader's position
    int64_t m_fetch_pos = 0;    // where the next chunk starts
    int64_t m_end_at = -1;      // the end of the file once a short chunk revealed it
    uint64_t m_generation = 0;  // bumped by seeks, so a fetch in flight is discarded
    std::exception_ptr m_error;
    bool m_stop = false;
    std::thread m_producer; // last: joined before the state above goes away
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
