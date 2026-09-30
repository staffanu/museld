// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_BYTESOURCE_H
#define AC3RF_DECODE_BYTESOURCE_H

#include <sys/types.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class Logger;

// Where an input reader gets its bytes from: a file, a fifo, a web server, a
// capture device on the network.  The readers only ever read forward and seek,
// so this is all a source has to do; what is behind it -- a descriptor, HTTP
// range requests, a UDP stream being put back together -- stays here.
class ByteSource {
public:
    virtual ~ByteSource() = default;

    // Up to `len` bytes into `buf`.  Returns how many, 0 at the end of the
    // stream, or -1 with errno set: EAGAIN means a live source has nothing yet
    // and the caller should try again after a short wait; anything else is an
    // error.
    virtual ssize_t read(uint8_t *buf, size_t len) = 0;

    // Positions like lseek, returning the new position, or -1 with errno set
    // (ESPIPE on a source that cannot seek).
    virtual int64_t seek(int64_t offset, int whence) = 0;

    // The length in bytes, or -1 when it is not known.
    virtual int64_t size() = 0;

    // The samples arrive as they are produced -- a fifo, stdin, a network
    // stream.  Such a source cannot seek, does not report an end of stream
    // while its producer may still come back, and the player must keep up
    // with it rather than the other way round.
    virtual bool isLive() const = 0;
};

// Input names are file paths or URLs:
//   http://host[:port]/path   a file on a web server, fetched with range requests
//   udp://[address]:port      the ethadc capture stream, received on that port
bool inputIsUrl(const std::string &name);
bool inputIsNetworkStream(const std::string &name); // udp://
// A fifo, stdin ("-" is not special here; see openStdinByteSource) or a network stream.
bool inputIsLive(const std::string &name);
// The size of the input in bytes, or -1 when unknown (a live input, or a
// server that does not say).
int64_t inputSize(const std::string &name);

// Opens an input by name.  `log` may be null; the network sources use it for
// their stream and loss reports.  Throws std::runtime_error when the input
// cannot be opened.
std::unique_ptr<ByteSource> openByteSource(const std::string &name, Logger *log = nullptr);

// The standard input, treated as live.
std::unique_ptr<ByteSource> openStdinByteSource();

#endif //AC3RF_DECODE_BYTESOURCE_H
