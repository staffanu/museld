// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include "ByteSource.h"
#include "EthadcByteSource.h"
#include "FileSeek.h"
#include "HttpByteSource.h"

#ifndef O_BINARY
#  define O_BINARY 0
#endif
// Windows has no FIFOs; is_fifo() is always false there, so the flag is never used.
#ifndef O_NONBLOCK
#  define O_NONBLOCK 0
#endif

namespace {

// A file descriptor: a regular file, a fifo or stdin.
class FdByteSource : public ByteSource {
public:
    FdByteSource(int fd, bool is_fifo) : m_fd(fd), m_is_fifo(is_fifo) {}

    // The standard descriptors are not ours to close.
    ~FdByteSource() override {
        if (m_fd > STDERR_FILENO)
            close(m_fd);
    }

    ssize_t read(uint8_t *buf, size_t len) override {
        const ssize_t r = ::read(m_fd, buf, len);
        // A fifo whose writer went away is not at its end: the capture rig
        // restarts its writer and playback carries on, so report "nothing
        // yet" and let the reader keep waiting.
        if (r == 0 && m_is_fifo) {
            errno = EAGAIN;
            return -1;
        }
        return r;
    }

    int64_t seek(int64_t offset, int whence) override {
        if (m_is_fifo) {
            errno = ESPIPE;
            return -1;
        }
        return seekFile(m_fd, offset, whence);
    }

    int64_t size() override {
        if (m_is_fifo)
            return -1;
        struct stat st{};
        if (fstat(m_fd, &st) != 0)
            return -1;
        return (int64_t)st.st_size;
    }

    bool isLive() const override { return m_is_fifo; }

private:
    int m_fd;
    bool m_is_fifo;
};

bool hasScheme(const std::string &name, const char *scheme) {
    return name.starts_with(scheme);
}

} // namespace

bool inputIsUrl(const std::string &name) {
    return hasScheme(name, "http://") || hasScheme(name, "udp://");
}

bool inputIsNetworkStream(const std::string &name) {
    return hasScheme(name, "udp://");
}

bool inputIsLive(const std::string &name) {
    if (inputIsNetworkStream(name))
        return true;
    if (inputIsUrl(name))
        return false;
    return std::filesystem::is_fifo(name);
}

int64_t inputSize(const std::string &name) {
    if (inputIsNetworkStream(name))
        return -1;
    if (hasScheme(name, "http://"))
        return HttpByteSource::sizeOf(name);
    std::error_code ec;
    const auto size = std::filesystem::file_size(name, ec);
    return ec ? -1 : (int64_t)size;
}

std::unique_ptr<ByteSource> openByteSource(const std::string &name, Logger *log) {
    if (hasScheme(name, "http://"))
        return std::make_unique<HttpByteSource>(name, log);
    if (inputIsNetworkStream(name))
        return std::make_unique<EthadcByteSource>(name, log);

    const bool is_fifo = std::filesystem::is_fifo(name);
    int flags = O_RDONLY | O_BINARY;
    if (is_fifo)
        flags |= O_NONBLOCK;

    const int fd = open(name.c_str(), flags);
    if (fd == -1)
        throw std::runtime_error(std::format("Unable to open input file {}: {}", name, strerror(errno)));

#ifdef linux
    if (is_fifo)
        fcntl(fd, F_SETPIPE_SZ, 1024 * 1024);
#endif
    return std::make_unique<FdByteSource>(fd, is_fifo);
}

std::unique_ptr<ByteSource> openStdinByteSource() {
    return std::make_unique<FdByteSource>(STDIN_FILENO, true);
}
