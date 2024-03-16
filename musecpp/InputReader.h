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
    struct InputReaderBlock {
        static constexpr int c_max_efm_data_size = 150000; // a bit above 7350 / 30 * 588;
        InputReaderBlock(std::shared_ptr<musevk::VulkanBuffer> v, std::shared_ptr<musevk::VulkanBuffer> d)
                : input_offset(0),
                  input_samples_per_muse_sample(0),
                  video_data(std::move(v)),
                  dropout_data(std::move(d)),
                  efm_data_size(0),
                  efm_data() {
        };
        long input_offset;
        double input_samples_per_muse_sample;
        std::shared_ptr<musevk::VulkanBuffer> video_data;
        std::shared_ptr<musevk::VulkanBuffer> dropout_data;
        int efm_data_size;
        std::array<bool, c_max_efm_data_size> efm_data;
    };

    void operator=(const InputReader&) = delete;
    InputReader(const InputReader&) = delete;
    virtual ~InputReader() = default;

    [[nodiscard]] virtual bool initialize(std::vector<std::unique_ptr<InputReaderBlock>> &buffers);
    virtual void cleanup();

    enum class InputStatus {
        eNormal = 0,
        eBuffersEmpty = 1,
        eBuffersFilled = 2, // buffers all filled, real-time input
        eTimeout = 3, // returned value is null
        eEof = 4, // returned value is null
    };

    std::pair<std::unique_ptr<InputReaderBlock>, InputStatus> getNextInputBuffer();
    void returnBuffer(std::unique_ptr<InputReaderBlock> &buffer);
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
    std::deque<std::unique_ptr<InputReaderBlock>> m_vacant_muse_input_buffers;
    std::deque<std::unique_ptr<InputReaderBlock>> m_filled_muse_input_buffers;
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
