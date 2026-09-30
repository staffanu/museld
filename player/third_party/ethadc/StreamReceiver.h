// SPDX-License-Identifier: GPL-3.0-or-later
//
// The socket side of receiving an ethadc stream: binds the UDP port, sizes the
// receive buffer, and runs the loop that feeds datagrams and the passage of
// time to a StreamReassembler.  The loop runs on the calling thread; the
// caller's poll callback decides when it ends and is where periodic work such
// as statistics belongs.

#pragma once

#include <sys/socket.h>
#include <sys/time.h>

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "Net.h"
#include "Protocol.h"
#include "StreamReassembler.h"

namespace ethadc {

struct ReceiverConfig {
    std::string bind = "0.0.0.0";
    std::string port;
    std::uint64_t rcvbuf = 32ull * 1024 * 1024;
    // How long a receive may block; the reassembler's timers and the poll
    // callback run at least this often when the sender goes quiet.
    std::chrono::microseconds poll_interval{2000};
};

class StreamReceiver {
public:
    StreamReceiver(const ReceiverConfig &config, StreamReassembler &reassembler)
        : m_reassembler(reassembler),
          m_sock(udp_socket({config.bind, config.port}, true)),
          m_buf(kHeaderSize + kMaxPayload) {
        const int wanted = static_cast<int>(
            std::min<std::uint64_t>(config.rcvbuf, static_cast<std::uint64_t>(INT_MAX)));
        m_granted = m_sock.set_buffer(SO_RCVBUF, wanted);

        timeval tv{};
        tv.tv_sec = static_cast<decltype(tv.tv_sec)>(config.poll_interval.count() / 1000000);
        tv.tv_usec = static_cast<decltype(tv.tv_usec)>(config.poll_interval.count() % 1000000);
        setsockopt(m_sock.fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }

    // The receive buffer the kernel actually granted, or -1 if unknown.  At
    // these rates the socket buffer is the first thing to lose packets, long
    // before the network does, so a caller should report it.
    int granted_rcvbuf() const { return m_granted; }

    // The sysctl that caps it on this platform.
    static const char *rcvbuf_knob() {
#ifdef __linux__
        return "net.core.rmem_max";
#else
        return "kern.ipc.maxsockbuf";
#endif
    }

    // Receives until `poll` returns false.  `poll` is called after every
    // datagram and after every receive timeout, with the current time, then
    // the reassembler is finished so nothing stays in its window.
    void run(const std::function<bool(TimePoint)> &poll) {
        for (;;) {
            const ssize_t n = recv(m_sock.fd(), m_buf.data(), m_buf.size(), 0);
            const TimePoint now = Clock::now();
            if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                    throw std::runtime_error(std::format("recv: {}", std::strerror(errno)));
                m_reassembler.tick(now);
            } else {
                m_reassembler.packet(m_buf.data(), static_cast<std::size_t>(n), now);
            }
            if (!poll(now))
                break;
        }
        m_reassembler.finish();
    }

private:
    StreamReassembler &m_reassembler;
    Socket m_sock;
    int m_granted = -1;
    std::vector<std::uint8_t> m_buf;
};

}  // namespace ethadc
