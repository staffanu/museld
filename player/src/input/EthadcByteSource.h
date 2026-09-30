// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_ETHADCBYTESOURCE_H
#define AC3RF_DECODE_ETHADCBYTESOURCE_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include "ByteSource.h"

class Logger;

// The ethadc capture stream: UDP datagrams of samples, put back in order and
// with their losses concealed by the vendored ethadc receiver
// (third_party/ethadc), delivered here as the continuous byte stream the
// readers expect.  A live source: "udp://[address]:port" binds that port and
// waits for whatever sends to it.
class EthadcByteSource : public ByteSource {
public:
    // What the stream's packets say about it.
    struct StreamInfo {
        std::string format_name; // u8, s8, u16, s16, u10p, s10p
        double sample_frequency; // Hz
    };

    // Binds the port and starts receiving.  Throws std::runtime_error when the
    // URL is malformed or the port cannot be bound.
    EthadcByteSource(const std::string &url, Logger *log);
    ~EthadcByteSource() override;

    EthadcByteSource(const EthadcByteSource &) = delete;
    EthadcByteSource &operator=(const EthadcByteSource &) = delete;

    ssize_t read(uint8_t *buf, size_t len) override;
    int64_t seek(int64_t offset, int whence) override;
    int64_t size() override { return -1; }
    bool isLive() const override { return true; }

    // Binds the port just long enough to see one packet, and reports its
    // header; nullopt when none arrived within `timeout`.  The port is free
    // again on return.
    static std::optional<StreamInfo> probe(const std::string &url, std::chrono::milliseconds timeout);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif //AC3RF_DECODE_ETHADCBYTESOURCE_H
