// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <format>
#include "musevk/VulkanBuffer.h"
#include "FrameReader.h"
#include "logging/Logger.h"

#ifndef O_BINARY
#  define O_BINARY 0
#endif

template<class InputBlock>
FrameReader<InputBlock>::FrameReader(Logger &log, const std::string &filename, bool input_is_realtime,
                         double initial_seek_seconds, const std::optional<std::string> &output_filename)
        : m_log(log),
          m_filename(filename),
          m_input_is_realtime(input_is_realtime),
          m_initial_seek_seconds(initial_seek_seconds),
          m_output_filename(output_filename),
          m_output_file_fd(-1),
          m_file_write_buffer(nullptr),
          m_vacant_input_buffers{},
          m_filled_input_buffers{},
          m_reader_thread(nullptr),
          m_reader_thread_finished(false),
          m_stop_request(false),
          m_mutex(),
          m_cv_filled(),
          m_cv_vacant(),
          m_get_input_buffers_count(0) {
}

template<class InputBlock>
bool FrameReader<InputBlock>::initialize(std::vector<std::unique_ptr<InputBlock>> &buffers) {
    if (m_initial_seek_seconds != 0)
        seek(m_initial_seek_seconds);

    // We take ownership of the input blocks here -- the only reason not to create them here is that
    // we do not have access to the VulkanManager.
    for (auto &b : buffers)
        m_vacant_input_buffers.push_back(std::move(b));
    buffers.clear();

    m_log.debug(eInput, std::format("Using {} input buffers", m_vacant_input_buffers.size()));

    m_reader_thread = new std::thread([this]() {
#ifdef __APPLE__
        pthread_setname_np("museld-reader");
#elif defined(linux)
        pthread_setname_np(pthread_self(), "museld-reader");
#endif
        threadFunc();
    });

    if (m_output_filename) {
        m_output_file_fd = open(m_output_filename.value().c_str(),
                                O_WRONLY | O_TRUNC | O_CREAT | O_BINARY,
                                S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (m_output_file_fd == -1)
            throw std::runtime_error(std::format("Unable to open output file for writing: {}",
                                            strerror(errno)));

        m_file_write_buffer = malloc(InputBlock::c_requiredFileWriteBufferSize);
        if (m_file_write_buffer == nullptr)
            throw std::runtime_error("Cannot allocate memory for file output buffer");
    }

    return true;
}

template<class InputBlock>
void FrameReader<InputBlock>::cleanup() {
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_stop_request = true;
        m_cv_vacant.notify_all();
    }
    m_reader_thread->join();
    delete m_reader_thread;
    m_vacant_input_buffers.clear();
    m_filled_input_buffers.clear();

    if (m_output_file_fd != -1) {
        close(m_output_file_fd);
        free(m_file_write_buffer);
    }
}

template<class InputBlock>
std::pair<std::unique_ptr<InputBlock>, InputStatus>
FrameReader<InputBlock>::getNextInputBuffer() {
    m_get_input_buffers_count++;
    std::unique_ptr<InputBlock> buffer = nullptr;
    InputStatus hint = InputStatus::eNormal;
    {
        std::unique_lock<std::mutex> lock(m_mutex);

        m_cv_filled.wait_for(
                lock,
                std::chrono::milliseconds(100),
                [this] { return m_reader_thread_finished || !m_filled_input_buffers.empty(); });

        if (m_filled_input_buffers.empty())
            return {nullptr, m_reader_thread_finished ? InputStatus::eEof : InputStatus::eTimeout};

        buffer = std::move(m_filled_input_buffers.front());
        m_filled_input_buffers.pop_front();

        auto filled_buffers = m_filled_input_buffers.size();
        if (filled_buffers == 0 && !m_reader_thread_finished) {
            m_log.warn(eInput, std::format("getNextInputBuffer: no filled buffers after this one"));
            hint = InputStatus::eBuffersEmpty;
        } else if (m_vacant_input_buffers.empty() && m_input_is_realtime) {
            m_log.warn(eInput, std::format("getNextInputBuffer: no vacant buffers available"));
            hint = InputStatus::eBuffersFilled;
        } else if (m_get_input_buffers_count % 30 == 0)
            m_log.debug(eInput, std::format("getNextInputBuffer: {} buffers filled", filled_buffers));
    }

    if (m_output_file_fd != -1)
        buffer->writeToFile(m_output_file_fd, m_file_write_buffer);

    return {std::move(buffer), hint};
}

template<class InputBlock>
void FrameReader<InputBlock>::returnBuffer(std::unique_ptr<InputBlock> &buffer) {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv_vacant.notify_one();
    m_vacant_input_buffers.push_back(std::move(buffer));
}
