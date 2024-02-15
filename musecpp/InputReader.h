//
// Created by staffanu on 6/25/23.
//

#ifndef MUSECPP_INPUTREADER_H
#define MUSECPP_INPUTREADER_H

#include <thread>
#include <condition_variable>
#include <optional>
#include <vector>
#include <atomic>
#include "InputPll.h"

namespace musevk {
    class VulkanBuffer;
}
class Logger;

class InputReader {
public:
    void operator=(const InputReader&) = delete;
    InputReader(const InputReader&) = delete;
    virtual ~InputReader() = default;
    [[nodiscard]] virtual bool initialize(std::vector<std::shared_ptr<musevk::VulkanBuffer>> const &buffers);
    virtual void cleanup();

    struct InputReaderBlock {
        static constexpr int c_max_efm_data_size = 150000; // a bit above 7350 / 30 * 588;
        explicit InputReaderBlock(std::shared_ptr<musevk::VulkanBuffer> v)
        : video_data(std::move(v)),
          efm_data_size(0),
          efm_data() {
        };
        std::shared_ptr<musevk::VulkanBuffer> video_data;
        int efm_data_size;
        std::array<bool, c_max_efm_data_size> efm_data;
    };

    enum PresentationHint {
        eNormal = 0,
        eSlowdown = 1,
        eSpeedup = 2
    };

    std::pair<std::shared_ptr<InputReaderBlock>, PresentationHint> getNextInputBuffer();
    void returnBuffer(std::shared_ptr<InputReaderBlock> &buffer);
    virtual void seek(double seconds) = 0;

protected:
    // input_is_realtime is separate from input_is_fifo since we could have non-real-time input from a pipe
    // that is written to by someone that reads from a file (the example so far is reading from the FmDemodulator)
    InputReader(Logger &log, const std::string &filename,
                bool input_is_realtime,
                double initial_seek_seconds, const std::optional<std::string> &output_filename);

    virtual void threadFunc() = 0;

    Logger &m_log;
    const std::string m_filename;
    const bool m_input_is_realtime; // if real-time, we tell the player to speed up if all buffers are full
    const double m_initial_seek_seconds;
    const std::optional<std::string> m_output_filename;
    int m_output_file_fd;
    int16_t *m_output_short_buffer;
    std::deque<std::shared_ptr<InputReaderBlock>> m_vacant_muse_input_buffers;
    std::deque<std::shared_ptr<InputReaderBlock>> m_filled_muse_input_buffers;
    std::atomic<bool> m_stop_request;
    std::atomic<bool> m_reader_thread_finished;
    std::mutex m_mutex;
    std::condition_variable m_cv_filled;
    std::condition_variable m_cv_vacant;
    int m_get_input_buffers_count;

private:
    std::thread *m_reader_thread;
};

#endif //MUSECPP_INPUTREADER_H
