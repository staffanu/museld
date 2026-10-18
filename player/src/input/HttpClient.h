// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

// Include this in place of "httplib.h": cpp-httplib's configuration macros must
// be the same in every translation unit that includes it (its functions are
// inline), and a definition with parentheses in it does not survive CMake's
// Xcode generator, so the one place for them is here.  (Not named HttpLib.h: on a
// case-insensitive file system "httplib.h" from this directory would find itself.)
#ifndef MUSECPP_HTTPCLIENT_H
#define MUSECPP_HTTPCLIENT_H

#include <cstddef>

// Response bodies are received in pieces of this size (the default is 16 KiB).
// The http:// input fetches 4 MiB chunks, and on the Apple TV 4K (2017) the
// default read 45 MB/s from a gigabit LAN where this reads 60-100 MB/s (the
// NAS varies run to run); a 62.5 MHz capture needs 62.5 MB/s as s8.  Receiving
// at 100 MB/s costs that three-core CPU enough that the GPU is starved of its
// driver's time: the demodulator's GPU chain makes little progress during the
// read, which it does not suffer at 62.5 MB/s.
#define CPPHTTPLIB_RECV_BUFSIZ std::size_t(262144u)

#include "httplib.h"

#endif //MUSECPP_HTTPCLIENT_H
