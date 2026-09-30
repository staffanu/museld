// SPDX-License-Identifier: GPL-3.0-or-later
//
// Small socket helpers shared by the transmitter and the receiver.

#pragma once

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ethadc {

// "host:port", "1.2.3.4:5000" or "[::1]:5000".
struct Endpoint {
    std::string host;
    std::string port;
};

inline Endpoint parse_endpoint(std::string_view s) {
    if (!s.empty() && s.front() == '[') {
        const auto close = s.find(']');
        if (close == std::string_view::npos || close + 1 >= s.size() || s[close + 1] != ':')
            throw std::runtime_error(std::format("malformed address '{}'", s));
        return {std::string(s.substr(1, close - 1)), std::string(s.substr(close + 2))};
    }
    const auto colon = s.rfind(':');
    if (colon == std::string_view::npos)
        throw std::runtime_error(std::format("address '{}' needs a :port", s));
    return {std::string(s.substr(0, colon)), std::string(s.substr(colon + 1))};
}

class Socket {
public:
    Socket() = default;
    explicit Socket(int fd) : m_fd(fd) {}
    ~Socket() {
        if (m_fd >= 0)
            ::close(m_fd);
    }
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    Socket(Socket &&other) noexcept : m_fd(other.m_fd) { other.m_fd = -1; }

    int fd() const { return m_fd; }

    // Sets the buffer and reports what the kernel actually granted.  On Linux
    // the request is silently clamped to net.core.rmem_max, which at the rates
    // this stream runs at is the difference between a clean capture and steady
    // loss that looks like a network fault.
    int set_buffer(int option, int bytes) {
        setsockopt(m_fd, SOL_SOCKET, option, &bytes, sizeof bytes);
        int granted = 0;
        socklen_t len = sizeof granted;
        if (getsockopt(m_fd, SOL_SOCKET, option, &granted, &len) != 0)
            return -1;
        // Linux reports double the usable size; halve it so the number printed
        // means what the caller asked for.
#ifdef __linux__
        granted /= 2;
#endif
        return granted;
    }

private:
    int m_fd = -1;
};

// Resolves to a UDP socket.  When `bind_it` is set the address is bound
// (receiver); otherwise it is connected (transmitter), which lets us use
// send() and gets ICMP port-unreachable reported back as an error.
inline Socket udp_socket(const Endpoint &ep, bool bind_it, sockaddr_storage *out_addr = nullptr) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    if (bind_it)
        hints.ai_flags = AI_PASSIVE;

    addrinfo *res = nullptr;
    const char *node = ep.host.empty() ? nullptr : ep.host.c_str();
    const int err = getaddrinfo(node, ep.port.c_str(), &hints, &res);
    if (err != 0)
        throw std::runtime_error(
            std::format("cannot resolve {}:{}: {}", ep.host, ep.port, gai_strerror(err)));

    for (addrinfo *ai = res; ai != nullptr; ai = ai->ai_next) {
        const int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        int ok;
        if (bind_it) {
            const int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            ok = ::bind(fd, ai->ai_addr, ai->ai_addrlen) == 0;
        } else {
            ok = ::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0;
        }
        if (ok) {
            if (out_addr != nullptr)
                std::memcpy(out_addr, ai->ai_addr, ai->ai_addrlen);
            freeaddrinfo(res);
            return Socket(fd);
        }
        ::close(fd);
    }
    const std::string what = std::strerror(errno);
    freeaddrinfo(res);
    throw std::runtime_error(std::format("cannot {} {}:{}: {}", bind_it ? "bind" : "connect",
                                         ep.host, ep.port, what));
}

}  // namespace ethadc
