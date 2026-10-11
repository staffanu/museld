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
#ifdef _WIN32
#  include <io.h>
#endif
#include "ByteSource.h"
#include "EthadcByteSource.h"
#include "FileSeek.h"
#include "HttpByteSource.h"

#ifndef O_BINARY
#  define O_BINARY 0
#endif
// Windows has no non-blocking descriptors; a pipe read there blocks until data
// or the end, which a pipe always delivers in the end.
#ifndef O_NONBLOCK
#  define O_NONBLOCK 0
#endif
#ifndef S_ISSOCK
#  define S_ISSOCK(m) 0
#endif

namespace {

constexpr const char *c_stdin_name = "-";

// A file descriptor: a regular file, a fifo or stdin.
class FdByteSource : public ByteSource {
public:
    // A live descriptor delivers its samples as they are produced.  Whether
    // its end is final tells a fifo opened by name (its writer can come back,
    // so the end is waited through) from a pipe on stdin (nobody can reopen
    // it once the writer is gone).
    FdByteSource(int fd, bool live, bool end_is_final) : m_fd(fd), m_live(live), m_end_is_final(end_is_final) {}

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
        if (r == 0 && m_live && !m_end_is_final) {
            errno = EAGAIN;
            return -1;
        }
        return r;
    }

    int64_t seek(int64_t offset, int whence) override {
        if (m_live) {
            errno = ESPIPE;
            return -1;
        }
        return seekFile(m_fd, offset, whence);
    }

    int64_t size() override {
        if (m_live)
            return -1;
        struct stat st{};
        if (fstat(m_fd, &st) != 0)
            return -1;
        return (int64_t)st.st_size;
    }

    bool isLive() const override { return m_live; }

private:
    int m_fd;
    bool m_live;
    bool m_end_is_final;
};

bool hasScheme(const std::string &name, const char *scheme) {
    return name.starts_with(scheme);
}

// What stdin is connected to, by its descriptor: the name "-" says nothing.
struct StdinKind {
    bool live = false;     // a pipe or a socket: samples arrive as they are produced
    bool regular = false;  // a file redirected in: seekable, with a size
    bool present = false;  // either of the above, as opposed to a terminal, /dev/null or nothing
};

StdinKind stdinKind() {
    StdinKind kind;
    struct stat st{};
    if (fstat(STDIN_FILENO, &st) != 0)
        return kind;
    kind.live = S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode);
    kind.regular = S_ISREG(st.st_mode);
    kind.present = kind.live || kind.regular;
    return kind;
}

} // namespace

bool inputIsUrl(const std::string &name) {
    return hasScheme(name, "http://") || hasScheme(name, "udp://");
}

bool inputIsNetworkStream(const std::string &name) {
    return hasScheme(name, "udp://");
}

bool inputIsStdin(const std::string &name) {
    return name == c_stdin_name;
}

bool stdinCarriesInput() {
    return stdinKind().present;
}

bool inputIsLive(const std::string &name) {
    if (inputIsNetworkStream(name))
        return true;
    if (inputIsUrl(name))
        return false;
    if (inputIsStdin(name))
        return stdinKind().live;
    return std::filesystem::is_fifo(name);
}

bool inputExists(const std::string &name) {
    if (inputIsUrl(name) || inputIsStdin(name))
        return true;
    return std::filesystem::exists(name);
}

int64_t inputSize(const std::string &name) {
    if (inputIsNetworkStream(name))
        return -1;
    if (hasScheme(name, "http://"))
        return HttpByteSource::sizeOf(name);
    if (inputIsStdin(name)) {
        struct stat st{};
        if (!stdinKind().regular || fstat(STDIN_FILENO, &st) != 0)
            return -1;
        return (int64_t)st.st_size;
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(name, ec);
    return ec ? -1 : (int64_t)size;
}

std::unique_ptr<ByteSource> openByteSource(const std::string &name, Logger *log) {
    if (hasScheme(name, "http://"))
        return std::make_unique<HttpByteSource>(name, log);
    if (inputIsNetworkStream(name))
        return std::make_unique<EthadcByteSource>(name, log);
    if (inputIsStdin(name))
        return openStdinByteSource();

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
    return std::make_unique<FdByteSource>(fd, is_fifo, false);
}

std::unique_ptr<ByteSource> openStdinByteSource() {
    const StdinKind kind = stdinKind();
#ifdef _WIN32
    // The C runtime opens the standard streams in text mode, which turns CRLF
    // into LF and ends the stream at a 0x1A byte; samples need the bytes as sent.
    _setmode(STDIN_FILENO, _O_BINARY);
#else
    // Polled like a fifo opened by name, so a reader thread waiting for data
    // notices a stop request instead of sitting in read() for good.  The
    // descriptor is the pipe's only read end, so nobody else sees the flag.
    if (kind.live)
        fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
#endif
    // A pipe or socket is live and its end is final (the writer that closed
    // it cannot be replaced); anything else -- a redirected file, mostly --
    // reads like the file it is.
    return std::make_unique<FdByteSource>(STDIN_FILENO, kind.live, true);
}
