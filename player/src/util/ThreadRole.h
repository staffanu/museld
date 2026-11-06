// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_THREADROLE_H
#define MUSELD_THREADROLE_H

#include <cstdlib>
#include <string>
#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#elif defined(linux) || defined(__FreeBSD__)
#include <pthread.h>
#endif

// Names the calling thread and, on Apple platforms, gives it the QoS class its
// role calls for.  The player's threads are not equal: the demodulator thread
// feeds the GPU and the reader thread and the main thread keep the picture
// moving, while the input threads -- the FLAC decoders, the Ogg scanner, the
// HTTP fetcher, the prefetcher -- run ahead of the picture by seconds' worth of
// buffer and can wait.  On an Apple TV 4K's three cores the FLAC decoders at
// equal priority starved the GPU driver of CPU (the chain's GPU wait rose from
// 5.6 to 7.5 ms per block and audio underran), so they run at utility QoS and
// the demodulator at user-interactive.  Elsewhere only the name is set.
enum class ThreadRole {
    eDemodulator, // feeds the GPU: user-interactive
    eReader,      // the frame reader: user-initiated
    eInput,       // decoding and fetching ahead of the reader: utility
};

inline void setThreadRole(ThreadRole role, const char *name) {
#ifdef __APPLE__
    pthread_setname_np(name);
    static const bool disabled = getenv("MUSELD_THREAD_QOS") != nullptr && getenv("MUSELD_THREAD_QOS")[0] == '0'; // A/B hook
    if (disabled)
        return;
    const qos_class_t qos = role == ThreadRole::eDemodulator ? QOS_CLASS_USER_INTERACTIVE
                          : role == ThreadRole::eReader ? QOS_CLASS_USER_INITIATED
                          : QOS_CLASS_UTILITY;
    pthread_set_qos_class_self_np(qos, 0);
#elif defined(linux) || defined(__FreeBSD__)
    (void)role;
    pthread_setname_np(pthread_self(), name);
#else
    (void)role;
    (void)name;
#endif
}

#endif //MUSELD_THREADROLE_H
