// SPDX-License-Identifier: GPL-3.0-or-later
//
// A bounded byte queue between the thread that reassembles the stream and the
// one that consumes it.
//
// The producer never blocks: if a push does not fit, the whole push is dropped
// and counted.  Without this a consumer that stalls for a frame would back the
// delay up into the socket, where it becomes packet loss indistinguishable from
// the network's; with it, it becomes an overflow counter that says plainly that
// the consumer could not keep up.  The consumer blocks, optionally with a
// timeout, until bytes are available or the ring is closed and drained.

#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

namespace ethadc {

class ByteRing {
public:
    explicit ByteRing(std::size_t capacity) : m_buf(capacity) {}

    std::size_t capacity() const { return m_buf.size(); }

    // Returns false, and drops everything, when `len` bytes do not fit.
    bool push(const std::uint8_t *data, std::size_t len) {
        {
            std::lock_guard lock(m_mutex);
            if (m_closed || m_used + len > m_buf.size()) {
                m_overflow_bytes += len;
                ++m_overflows;
                return false;
            }
            const std::size_t first = std::min(len, m_buf.size() - m_head);
            std::memcpy(m_buf.data() + m_head, data, first);
            std::memcpy(m_buf.data(), data + first, len - first);
            m_head = (m_head + len) % m_buf.size();
            m_used += len;
            m_high_water = std::max(m_high_water, m_used);
        }
        m_ready.notify_one();
        return true;
    }

    // Copies up to `max` bytes out, waiting for at least one.  Returns 0 when
    // the ring is closed and empty, or when `timeout` passes with nothing to
    // hand out; closed() tells the two apart.
    std::size_t pop(std::uint8_t *out, std::size_t max,
                    std::optional<std::chrono::steady_clock::duration> timeout = std::nullopt) {
        std::unique_lock lock(m_mutex);
        const auto ready = [this] { return m_used > 0 || m_closed; };
        if (timeout) {
            if (!m_ready.wait_for(lock, *timeout, ready))
                return 0;
        } else {
            m_ready.wait(lock, ready);
        }
        if (m_used == 0)
            return 0;
        const std::size_t take = std::min(max, m_used);
        const std::size_t first = std::min(take, m_buf.size() - m_tail);
        std::memcpy(out, m_buf.data() + m_tail, first);
        std::memcpy(out + first, m_buf.data(), take - first);
        m_tail = (m_tail + take) % m_buf.size();
        m_used -= take;
        return take;
    }

    // No more pushes are accepted; pops drain what is there and then return 0.
    void close() {
        {
            std::lock_guard lock(m_mutex);
            m_closed = true;
        }
        m_ready.notify_all();
    }

    bool closed() {
        std::lock_guard lock(m_mutex);
        return m_closed;
    }

    std::size_t used() {
        std::lock_guard lock(m_mutex);
        return m_used;
    }

    std::size_t high_water() {
        std::lock_guard lock(m_mutex);
        return m_high_water;
    }

    std::uint64_t overflow_bytes() {
        std::lock_guard lock(m_mutex);
        return m_overflow_bytes;
    }

    std::uint64_t overflows() {
        std::lock_guard lock(m_mutex);
        return m_overflows;
    }

private:
    std::vector<std::uint8_t> m_buf;
    std::size_t m_head = 0, m_tail = 0, m_used = 0, m_high_water = 0;
    std::uint64_t m_overflow_bytes = 0, m_overflows = 0;
    bool m_closed = false;
    std::mutex m_mutex;
    std::condition_variable m_ready;
};

}  // namespace ethadc
