// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cassert>
#include <cstring>
#include <pthread.h>
#include "PrefetchingInputReader.h"

PrefetchingInputReader::PrefetchingInputReader(std::unique_ptr<InputReader> inner, int queue_depth)
    // The base class fd is never used here; every file operation goes through the inner reader.
    : InputReader(-1, inner->block_size(), inner->is_fifo()),
      m_inner(std::move(inner)),
      m_queue_depth(queue_depth) {
    assert(m_queue_depth > 0);
}

PrefetchingInputReader::~PrefetchingInputReader() {
    {
        std::scoped_lock<std::mutex> lock(m_mutex);
        m_stop = true;
        m_cv.notify_all();
    }
    if (m_producer.joinable())
        m_producer.join();
}

void PrefetchingInputReader::initialize() {
    m_inner->initialize();
    m_producer = std::thread([this] { produce(); });
}

void PrefetchingInputReader::produce() {
#ifdef __APPLE__
    pthread_setname_np("input-prefetch");
#elif defined(linux)
    pthread_setname_np(pthread_self(), "input-prefetch");
#endif
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_stop) {
        if (m_seek_pending || m_eof || (int)m_filled.size() >= m_queue_depth) {
            // Parking at a block boundary is what makes the seek arithmetic exact: with no
            // read in flight, the inner reader is ahead by exactly the blocks in m_filled.
            m_producer_parked = true;
            m_cv.notify_all();
            m_cv.wait(lock);
            m_producer_parked = false;
            continue;
        }
        std::vector<float> block;
        if (!m_vacant.empty()) {
            block = std::move(m_vacant.back());
            m_vacant.pop_back();
        }
        lock.unlock();
        block.resize(m_block_size);
        int got = 0;
        std::exception_ptr error;
        try {
            got = m_inner->readFloats(block.data());
        } catch (...) {
            error = std::current_exception();
        }
        lock.lock();
        if (error) {
            m_producer_exception = error;
            m_cv.notify_all();
            return;
        }
        if (got == 0) {
            // Stay around rather than exit: a later seek clears m_eof and reading resumes,
            // matching an unwrapped reader.  The consumer may not even have seen the end
            // yet -- the queue can still hold blocks.
            m_eof = true;
            m_vacant.push_back(std::move(block));
        } else {
            assert(got == (int)m_block_size);
            m_inner_position += got;
            m_filled.push_back(std::move(block));
        }
        m_cv.notify_all();
    }
}

int PrefetchingInputReader::readFloats(float *f) {
    std::vector<float> block;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return !m_filled.empty() || m_eof || m_producer_exception; });
        if (m_filled.empty()) {
            // Deliver the blocks decoded before the failure first; only then report it.
            if (m_producer_exception)
                std::rethrow_exception(m_producer_exception);
            return 0;
        }
        block = std::move(m_filled.front());
        m_filled.pop_front();
    }
    memcpy(f, block.data(), m_block_size * sizeof(float));
    std::scoped_lock<std::mutex> lock(m_mutex);
    m_vacant.push_back(std::move(block));
    m_cv.notify_all();
    return (int)m_block_size;
}

void PrefetchingInputReader::seek(off_t no_samples) {
    if (m_is_fifo)
        return;
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_producer.joinable()) {
        // Before initialize() nothing has been read ahead, so pass the request through.
        m_inner->seek(no_samples);
        return;
    }
    m_seek_pending = true;
    m_cv.notify_all();
    m_cv.wait(lock, [this] { return m_producer_parked || m_producer_exception; });
    // After a producer failure the queued exception is the story; readFloats reports it.
    if (!m_producer_exception) {
        try {
            off_t adjusted = no_samples - (off_t)m_filled.size() * (off_t)m_block_size;
            if (m_inner_position + adjusted < 0)
                adjusted = -m_inner_position; // clamp to the start of the stream
            m_inner->seek(adjusted);
            m_inner_position += adjusted;
            for (auto &block : m_filled)
                m_vacant.push_back(std::move(block));
            m_filled.clear();
            m_eof = false;
        } catch (...) {
            m_seek_pending = false;
            m_cv.notify_all();
            throw;
        }
    }
    m_seek_pending = false;
    m_cv.notify_all();
}
