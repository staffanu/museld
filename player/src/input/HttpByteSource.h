// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_HTTPBYTESOURCE_H
#define AC3RF_DECODE_HTTPBYTESOURCE_H

#include <cstdint>
#include <memory>
#include <string>
#include "ByteSource.h"

class Logger;

// A file on a web server, read with HTTP range requests so that seeking works.
// The bytes are fetched in large chunks by a thread running ahead of the
// reader, since a request per block would leave the decoder waiting on the
// network's round trip at every one of them.  Plain http only: the point is a
// NAS or a capture machine on the local network, and nothing here is secret.
class HttpByteSource : public ByteSource {
public:
    // Throws std::runtime_error when the URL is malformed or the server cannot
    // be reached or does not honour range requests.
    HttpByteSource(const std::string &url, Logger *log);
    ~HttpByteSource() override;

    HttpByteSource(const HttpByteSource &) = delete;
    HttpByteSource &operator=(const HttpByteSource &) = delete;

    ssize_t read(uint8_t *buf, size_t len) override;
    int64_t seek(int64_t offset, int whence) override;
    int64_t size() override;
    bool isLive() const override { return false; }

    // The Content-Length the server reports for `url`, or -1.
    static int64_t sizeOf(const std::string &url);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif //AC3RF_DECODE_HTTPBYTESOURCE_H
