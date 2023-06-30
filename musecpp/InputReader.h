//
// Created by staffanu on 6/25/23.
//

#ifndef MUSECPP_INPUTREADER_H
#define MUSECPP_INPUTREADER_H

#include <thread>
#include <condition_variable>
#include "InputPll.h"
#include "musevk/VulkanBuffer.h"

class InputReader {
public:
    [[nodiscard]] virtual bool initialize(std::vector<std::shared_ptr<musevk::VulkanBuffer>> const &buffers);
    virtual void cleanup();

    std::shared_ptr<musevk::VulkanBuffer> getNextInputBuffer();
    void returnBuffer(std::shared_ptr<musevk::VulkanBuffer> &buffer);

protected:
    InputReader(const std::string &filename, bool stop_on_eof);

    virtual void threadFunc() = 0;

    std::string m_filename;
    bool m_stop_on_eof;
    std::deque<std::shared_ptr<musevk::VulkanBuffer>> m_vacant_muse_input_buffers;
    std::deque<std::shared_ptr<musevk::VulkanBuffer>> m_filled_muse_input_buffers;
    bool m_stop_request;
    bool m_reader_thread_finished;
    std::mutex m_mutex;
    std::condition_variable m_cv_filled;
    std::condition_variable m_cv_vacant;

private:
    std::thread *m_reader_thread;
};

#endif //MUSECPP_INPUTREADER_H
