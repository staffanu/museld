// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <atomic>
#include <cerrno>
#include <cstring>
#include <pthread.h>
#include <exception>
#include <format>
#include <stdexcept>
#include <thread>
#include "EthadcByteSource.h"
#include "logging/Logger.h"

#ifndef _WIN32

#include "ByteRing.h"
#include "Net.h"
#include "Protocol.h"
#include "StreamReassembler.h"
#include "StreamReceiver.h"

namespace {

// Half a second of a 62.5 MHz 8-bit stream: what the decoder may fall behind
// before samples are dropped, and the largest the ring needs to be.
constexpr size_t c_ring_bytes = 32u << 20;
constexpr auto c_read_wait = std::chrono::milliseconds(20);
constexpr auto c_stats_interval = std::chrono::seconds(10);

// udp://[address]:port -- the address to bind, empty for all interfaces.
ethadc::ReceiverConfig configFor(const std::string &url) {
    constexpr const char *scheme = "udp://";
    if (!url.starts_with(scheme))
        throw std::runtime_error(std::format("Not a udp:// URL: {}", url));
    ethadc::Endpoint ep = ethadc::parse_endpoint(url.substr(strlen(scheme)));
    ethadc::ReceiverConfig config;
    config.bind = ep.host.empty() ? "0.0.0.0" : ep.host;
    config.port = ep.port;
    if (config.port.empty())
        throw std::runtime_error(std::format("No port in {}: use udp://[address]:port", url));
    return config;
}

} // namespace

struct EthadcByteSource::Impl {
    Impl(const std::string &url, Logger *log)
        : m_log(log),
          m_ring(c_ring_bytes),
          m_reassembler(ethadc::ReassemblerOptions{},
                        [this](const uint8_t *data, size_t len) { m_ring.push(data, len); },
                        [this](const ethadc::StreamInfo &info) { onStream(info); }),
          m_receiver(configFor(url), m_reassembler) {
        if (m_log) {
            const int granted = m_receiver.granted_rcvbuf();
            const ethadc::ReceiverConfig config = configFor(url);
            std::string note;
            if (granted >= 0 && (uint64_t)granted < config.rcvbuf)
                note = std::format(" (asked for {} MiB; raise {} to avoid loss under load)",
                                   config.rcvbuf >> 20, ethadc::StreamReceiver::rcvbuf_knob());
            m_log->info(eInput, std::format("Listening for the ethadc stream on {}:{}, receive buffer {} KiB{}",
                                            config.bind, config.port, granted >> 10, note));
        }
        m_thread = std::thread([this] { run(); });
    }

    ~Impl() {
        m_stop = true;
        if (m_thread.joinable())
            m_thread.join();
    }

    void onStream(const ethadc::StreamInfo &info) {
        if (m_log)
            m_log->info(eInput, std::format("ethadc stream {}: {} samples at {:.4g} MHz, {} bytes per packet",
                                            m_streams++ == 0 ? "started" : "restarted",
                                            ethadc::format_info(info.format).name, info.sample_rate / 1e6,
                                            info.payload_len));
    }

    void logStats() {
        if (!m_log)
            return;
        const auto &st = m_reassembler.stats();
        const uint64_t expected = st.expected();
        const uint64_t overflow = m_ring.overflow_bytes();
        const std::string line = std::format(
                "ethadc stream: {} packets, lost {} ({:.4f}%), reordered {}, duplicate {}, late {}, "
                "longest gap {} packets, {} bytes dropped by the decoder falling behind",
                st.packets, st.lost, expected > 0 ? 100.0 * (double)st.lost / (double)expected : 0.0,
                st.recovered, st.duplicates, st.too_late, st.longest_gap, overflow);
        // Loss is worth a warning the first time it grows; the rest is bookkeeping.
        if (st.lost > m_reported_lost || overflow > m_reported_overflow)
            m_log->warn(eInput, line);
        else
            m_log->info(eInput, line);
        m_reported_lost = st.lost;
        m_reported_overflow = overflow;
    }

    void run() {
#ifdef __APPLE__
        pthread_setname_np("input-ethadc");
#elif defined(linux) || defined(__FreeBSD__)
        pthread_setname_np(pthread_self(), "input-ethadc");
#endif
        try {
            auto last_stats = ethadc::Clock::now();
            m_receiver.run([&](ethadc::TimePoint now) {
                if (m_stop)
                    return false;
                if (m_reassembler.ended()) {
                    if (m_log)
                        m_log->info(eInput, "ethadc stream: the sender ended the stream");
                    return false;
                }
                if (now - last_stats >= c_stats_interval && m_reassembler.have_stream()) {
                    logStats();
                    last_stats = now;
                }
                return true;
            });
        } catch (...) {
            m_error = std::current_exception();
        }
        if (m_reassembler.have_stream())
            logStats();
        m_ring.close();
    }

    ssize_t read(uint8_t *buf, size_t len) {
        const size_t n = m_ring.pop(buf, len, c_read_wait);
        if (n > 0)
            return (ssize_t)n;
        if (m_error)
            std::rethrow_exception(m_error);
        if (m_ring.closed())
            return 0; // the stream ended, or we are shutting down
        errno = EAGAIN;
        return -1;
    }

    Logger *m_log;
    int m_streams = 0;
    uint64_t m_reported_lost = 0, m_reported_overflow = 0;
    std::atomic<bool> m_stop{false};
    std::exception_ptr m_error;
    ethadc::ByteRing m_ring;
    ethadc::StreamReassembler m_reassembler;
    ethadc::StreamReceiver m_receiver;
    std::thread m_thread; // last: joined before the members it uses go away
};

EthadcByteSource::EthadcByteSource(const std::string &url, Logger *log) : m_impl(std::make_unique<Impl>(url, log)) {}

EthadcByteSource::~EthadcByteSource() = default;

ssize_t EthadcByteSource::read(uint8_t *buf, size_t len) {
    return m_impl->read(buf, len);
}

int64_t EthadcByteSource::seek(int64_t, int) {
    errno = ESPIPE;
    return -1;
}

std::optional<EthadcByteSource::StreamInfo> EthadcByteSource::probe(const std::string &url,
                                                                    std::chrono::milliseconds timeout) {
    std::optional<StreamInfo> info;
    ethadc::StreamReassembler reassembler(
            ethadc::ReassemblerOptions{}, [](const uint8_t *, size_t) {},
            [&](const ethadc::StreamInfo &stream) {
                info = StreamInfo{ethadc::format_info(stream.format).name, (double)stream.sample_rate};
            });
    ethadc::StreamReceiver receiver(configFor(url), reassembler);
    const auto deadline = ethadc::Clock::now() + timeout;
    receiver.run([&](ethadc::TimePoint now) { return !info && now < deadline; });
    return info;
}

#else // _WIN32: the vendored receiver uses POSIX sockets, which MinGW does not have

struct EthadcByteSource::Impl {};

EthadcByteSource::EthadcByteSource(const std::string &url, Logger *) {
    throw std::runtime_error(std::format("{}: the udp:// input is not available in the Windows build", url));
}

EthadcByteSource::~EthadcByteSource() = default;

ssize_t EthadcByteSource::read(uint8_t *, size_t) {
    errno = EBADF;
    return -1;
}

int64_t EthadcByteSource::seek(int64_t, int) {
    errno = ESPIPE;
    return -1;
}

std::optional<EthadcByteSource::StreamInfo> EthadcByteSource::probe(const std::string &, std::chrono::milliseconds) {
    return std::nullopt;
}

#endif
