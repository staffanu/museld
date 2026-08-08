// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_FILESEEK_H
#define AC3RF_DECODE_FILESEEK_H

#include <cstdint>
#ifdef _WIN32
#  include <io.h>
#else
#  include <unistd.h>
#endif

// 64-bit file seek regardless of platform: on Windows (MinGW) off_t and lseek
// are 32-bit, which silently truncates positions in captures over 2 GB.
static inline int64_t seekFile(int fd, int64_t offset, int whence) {
#ifdef _WIN32
    return _lseeki64(fd, offset, whence);
#else
    return lseek(fd, offset, whence);
#endif
}

#endif //AC3RF_DECODE_FILESEEK_H
