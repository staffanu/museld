//
// Created by staffanu on 6/25/23.
//

#include <fmt/format.h>
#include "InputReader.h"

using namespace std;

InputReader::InputReader(Logger &log, const std::string &filename, bool input_is_fifo, double initial_seek_seconds)
        : m_log(log),
          m_filename(filename),
          m_input_is_fifo(input_is_fifo),
          m_initial_seek_seconds(initial_seek_seconds),
          m_vacant_muse_input_buffers{},
          m_filled_muse_input_buffers{},
          m_reader_thread(nullptr),
          m_reader_thread_finished(false),
          m_stop_request(false),
          m_mutex(),
          m_cv_filled(),
          m_cv_vacant(),
          m_get_input_buffers_count(0) {
}

bool InputReader::initialize(std::vector<std::shared_ptr<musevk::VulkanBuffer>> const &buffers) {
    for (const auto &b : buffers)
        m_vacant_muse_input_buffers.push_back(b);

    m_reader_thread = new thread(&InputReader::threadFunc, this);

    return true;
}

void InputReader::cleanup() {
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv_vacant.notify_one();
        m_stop_request = true;
    }
    m_reader_thread->join();
    m_vacant_muse_input_buffers.clear();
    m_filled_muse_input_buffers.clear();
}

pair<shared_ptr<musevk::VulkanBuffer>, InputReader::PresentationHint>
InputReader::getNextInputBuffer() {
    m_get_input_buffers_count++;

    std::unique_lock<std::mutex> lock(m_mutex);

    m_cv_filled.wait(
            lock,
            [this]{return m_reader_thread_finished || !m_filled_muse_input_buffers.empty();});

    if (m_reader_thread_finished)
        return {nullptr, eNormal};

    auto buffer = m_filled_muse_input_buffers.front();
    m_filled_muse_input_buffers.pop_front();

    PresentationHint hint = eNormal;
    auto filled_buffers = m_filled_muse_input_buffers.size();
    if (filled_buffers == 0) {
        m_log.warn(eInput, fmt::format("getNextInputBuffer: no filled buffers after this one"));
        hint = eSlowdown;
    } else if (m_vacant_muse_input_buffers.empty() && m_input_is_fifo) {
        m_log.warn(eInput, fmt::format("getNextInputBuffer: no vacant buffers available"));
        hint = eSpeedup;
    } else if (m_get_input_buffers_count % 30 == 0)
        m_log.debug(eInput, fmt::format("getNextInputBuffer: {} buffers filled", filled_buffers));

    return {buffer, hint};
}

void InputReader::returnBuffer(std::shared_ptr<musevk::VulkanBuffer> &buffer) {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv_vacant.notify_one();
    m_vacant_muse_input_buffers.push_back(buffer);
}
