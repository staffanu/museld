//
// Created by staffanu on 6/25/23.
//

#ifndef MUSECPP_FRAMEREADER_H
#define MUSECPP_FRAMEREADER_H

#include <thread>
#include <condition_variable>
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
    virtual ~FrameReader() = default;

    [[nodiscard]] virtual bool initialize(std::vector<std::unique_ptr<InputBlock>> &buffers);
    virtual void cleanup();

    std::pair<std::unique_ptr<InputBlock>, InputStatus> getNextInputBuffer();
    void returnBuffer(std::unique_ptr<InputBlock> &buffer);
    virtual void seek(double seconds) = 0;

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
    std::thread *m_reader_thread;
    void *m_file_write_buffer;
};

#include "FrameReader.impl.h"

#endif //MUSECPP_FRAMEREADER_H
