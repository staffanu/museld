//
// Created by staffanu on 6/25/23.
//

#include <sys/fcntl.h>
#include <unistd.h>
#include <fmt/format.h>
#include "musevk/VulkanBuffer.h"
#include "MuseTypes.h"
#include "InputReader.h"
#include "util/Logger.h"

using namespace std;

InputReader::InputReader(Logger &log, const std::string &filename, bool input_is_realtime,
                         double initial_seek_seconds, const std::optional<std::string> &output_filename)
        : m_log(log),
          m_filename(filename),
          m_input_is_realtime(input_is_realtime),
          m_initial_seek_seconds(initial_seek_seconds),
          m_output_filename(output_filename),
          m_output_file_fd(-1),
          m_output_short_buffer(nullptr),
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

bool InputReader::initialize(std::vector<std::unique_ptr<InputReaderBlock>> &buffers) {
    if (m_initial_seek_seconds != 0)
        seek(m_initial_seek_seconds);

    // We take ownership of the input blocks here -- the only reason not to create them here is that
    // we do not have access to the VulkanManager.
    for (auto &b : buffers)
        m_vacant_muse_input_buffers.push_back(std::move(b));
    buffers.clear();

    m_log.debug(eInput, fmt::format("Using {} input buffers", m_vacant_muse_input_buffers.size()));

    m_reader_thread = new thread(&InputReader::threadFunc, this);
#ifdef linux
    pthread_setname_np(m_reader_thread->native_handle(), "musecpp-reader");
#endif

    if (m_output_filename.has_value()) {
        m_output_file_fd = open(m_output_filename.value().c_str(),
                                O_WRONLY | O_TRUNC | O_CREAT,
                                S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (m_output_file_fd == -1)
            throw runtime_error(fmt::format("Unable to open output file for writing: {}",
                                            strerror(errno)));

        m_output_short_buffer = (int16_t *)malloc(sizeof(uint16_t) * MUSE_TOTAL_WIDTH * MUSE_TOTAL_HEIGHT);
        if (m_output_short_buffer == NULL)
            throw runtime_error("Cannot allocate memory for file output buffer");
    }

    return true;
}

void InputReader::cleanup() {
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv_vacant.notify_one();
        m_stop_request = true;
    }
    m_reader_thread->join();
    delete m_reader_thread;
    m_vacant_muse_input_buffers.clear();
    m_filled_muse_input_buffers.clear();

    if (m_output_file_fd != -1) {
        close(m_output_file_fd);
        free(m_output_short_buffer);
    }
}

pair<unique_ptr<InputReader::InputReaderBlock>, InputReader::InputStatus>
InputReader::getNextInputBuffer() {
    m_get_input_buffers_count++;
    unique_ptr<InputReaderBlock> buffer = nullptr;
    InputStatus hint = InputStatus::eNormal;
    {
        unique_lock<std::mutex> lock(m_mutex);

        m_cv_filled.wait_for(
                lock,
                chrono::microseconds(100),
                [this] { return m_reader_thread_finished || !m_filled_muse_input_buffers.empty(); });

        if (m_filled_muse_input_buffers.empty())
            return {nullptr, m_reader_thread_finished ? InputStatus::eEof : InputStatus::eTimeout};

        buffer = std::move(m_filled_muse_input_buffers.front());
        m_filled_muse_input_buffers.pop_front();

        auto filled_buffers = m_filled_muse_input_buffers.size();
        if (filled_buffers == 0 && !m_reader_thread_finished) {
            m_log.warn(eInput, fmt::format("getNextInputBuffer: no filled buffers after this one"));
            hint = InputStatus::eBuffersEmpty;
        } else if (m_vacant_muse_input_buffers.empty() && m_input_is_realtime) {
            m_log.warn(eInput, fmt::format("getNextInputBuffer: no vacant buffers available"));
            hint = InputStatus::eBuffersFilled;
        } else if (m_get_input_buffers_count % 30 == 0)
            m_log.debug(eInput, fmt::format("getNextInputBuffer: {} buffers filled", filled_buffers));
    }

    if (m_output_file_fd != -1) {
        int size = buffer->video_data->size().numberOfElements();
        float *ptr = buffer->video_data->data<float>();
        for (int i = 0; i < size; i++)
            m_output_short_buffer[i] = ptr[i] * 4;

        if (write(m_output_file_fd, m_output_short_buffer, size * sizeof(int16_t)) != size * sizeof(int16_t))
            throw runtime_error("Output file write error");
    }

    return {std::move(buffer), hint};
}

void InputReader::returnBuffer(unique_ptr<InputReader::InputReaderBlock> &buffer) {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv_vacant.notify_one();
    m_vacant_muse_input_buffers.push_back(std::move(buffer));
}
