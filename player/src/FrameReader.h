// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_FRAMEREADER_H
#define MUSECPP_FRAMEREADER_H

#include <thread>
#include <condition_variable>
#include <exception>
#include <optional>
#include <vector>
#include <deque>
#include <atomic>

namespace musevk {
    class VulkanBuffer;
}
class Logger;

enum class InputStatus {
    eNormal = 0,
    eBuffersEmpty = 1,
    eBuffersFilled = 2, // buffers all filled, real-time input
    eTimeout = 3, // returned value is null
    eEof = 4, // returned value is null
};

template<class InputBlock>
class FrameReader {
public:
    void operator=(const FrameReader&) = delete;
    FrameReader(const FrameReader&) = delete;
    // Derived classes call cleanup() from their own destructors, so the reader
    // thread is joined while the threadFunc() override it is running still
    // exists.  The call here is the idempotent backstop (and resolves to the
    // base cleanup(), which is why it cannot replace the derived calls).
    virtual ~FrameReader();

    [[nodiscard]] virtual bool initialize(std::vector<std::unique_ptr<InputBlock>> &buffers);
    virtual void cleanup();

    std::pair<std::unique_ptr<InputBlock>, InputStatus> getNextInputBuffer();
    void returnBuffer(std::unique_ptr<InputBlock> &buffer);
    virtual void seek(double seconds) = 0;
    virtual void setEfmEnabled(bool) {}
    // CX noise reduction for the NTSC analog audio; a no-op elsewhere
    virtual void setAnalogCx(bool) {}

protected:
    // input_is_realtime is separate from input_is_fifo since we could have non-real-time input from a pipe
    // that is written to by someone that reads from a file (the example so far is reading from the FmDemodulator)
    FrameReader(Logger &log, const std::string &filename,
                bool input_is_realtime,
                double initial_seek_seconds, const std::optional<std::string> &output_filename);

    virtual void threadFunc() = 0;

    Logger &m_log;
    const std::string m_filename;
    const bool m_input_is_realtime; // if real-time, we tell the player to speed up if all buffers are full
    const double m_initial_seek_seconds;
    const std::optional<std::string> m_output_filename;
    int m_output_file_fd;
    std::deque<std::unique_ptr<InputBlock>> m_vacant_input_buffers;
    std::deque<std::unique_ptr<InputBlock>> m_filled_input_buffers;
    std::atomic<bool> m_stop_request;
    std::atomic<bool> m_reader_thread_finished;
    std::mutex m_mutex;
    std::condition_variable m_cv_filled;
    std::condition_variable m_cv_vacant;
    int m_get_input_buffers_count;

private:
    void *m_file_write_buffer;
    std::exception_ptr m_thread_exception; // set by the reader thread; guarded by m_mutex
    std::thread m_reader_thread; // last member: joined before the state above goes away
};

#include "FrameReader.impl.h"

#endif //MUSECPP_FRAMEREADER_H
