// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef LDCONV_FILEIO_H
#define LDCONV_FILEIO_H

#include <cstdint>
#include <cstdio>
#include <string>

// Captures run to tens of gigabytes, so every offset here is 64-bit even where
// the platform's own long is not.
int seekFile(FILE *file, int64_t offset, int whence);
int64_t tellFile(FILE *file);

// Size in bytes, or -1 when the stream is a pipe or otherwise not a plain file.
int64_t fileSize(FILE *file);

// "-" opens stdin/stdout, in binary mode on the platforms that distinguish it.
FILE *openInput(const std::string &path);
FILE *openOutput(const std::string &path);

bool isStdStream(FILE *file);

// Reads until the buffer is full or the stream ends; a short count therefore
// means end of input rather than a partial read that should be retried.
size_t readFull(FILE *file, void *buffer, size_t bytes);
void writeFull(FILE *file, const void *buffer, size_t bytes);

#endif //LDCONV_FILEIO_H
