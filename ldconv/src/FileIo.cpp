// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cerrno>
#include <cstring>
#include <format>
#include <stdexcept>
#include <sys/stat.h>
#include "FileIo.h"

#ifdef _WIN32
#  include <fcntl.h>
#  include <io.h>
#endif

int seekFile(FILE *file, int64_t offset, int whence) {
#ifdef _WIN32
    return _fseeki64(file, offset, whence);
#else
    return fseeko(file, (off_t)offset, whence);
#endif
}

int64_t tellFile(FILE *file) {
#ifdef _WIN32
    return _ftelli64(file);
#else
    return (int64_t)ftello(file);
#endif
}

int64_t fileSize(FILE *file) {
#ifdef _WIN32
    struct __stat64 st{};
    if (_fstat64(_fileno(file), &st) != 0 || (st.st_mode & _S_IFREG) == 0)
        return -1;
#else
    struct stat st{};
    if (fstat(fileno(file), &st) != 0 || !S_ISREG(st.st_mode))
        return -1;
#endif
    return (int64_t)st.st_size;
}

FILE *openInput(const std::string &path) {
    if (path == "-") {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
#endif
        return stdin;
    }
    FILE *file = fopen(path.c_str(), "rb");
    if (file == nullptr)
        throw std::runtime_error(std::format("Cannot open {}: {}", path, strerror(errno)));
    return file;
}

FILE *openOutput(const std::string &path) {
    if (path == "-") {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        return stdout;
    }
    // "w+b" rather than "wb": libFLAC re-reads the first Ogg page at the end of
    // the encode to correct the header, which a write-only stream refuses.
    FILE *file = fopen(path.c_str(), "w+b");
    if (file == nullptr)
        throw std::runtime_error(std::format("Cannot create {}: {}", path, strerror(errno)));
    return file;
}

bool isStdStream(FILE *file) {
    return file == stdin || file == stdout || file == stderr;
}

size_t readFull(FILE *file, void *buffer, size_t bytes) {
    size_t done = 0;
    auto *p = (unsigned char *)buffer;
    while (done < bytes) {
        const size_t n = fread(p + done, 1, bytes - done, file);
        if (n == 0) {
            if (ferror(file))
                throw std::runtime_error(std::format("Read error: {}", strerror(errno)));
            break; // end of file
        }
        done += n;
    }
    return done;
}

void writeFull(FILE *file, const void *buffer, size_t bytes) {
    if (fwrite(buffer, 1, bytes, file) != bytes)
        throw std::runtime_error(std::format("Write error: {}", strerror(errno)));
}
