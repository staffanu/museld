//
// Created by staffanu on 12/10/23.
//

#include <thread>
#include <vector>
#include <iostream>
#include <filesystem>
#include "RfDemodulator.h"
#include "musevk/VulkanUtil.h"
#include "musevk/TimestampQueryPool.h"
#include "musevk/TimestampStatistics.h"

using namespace std;
using namespace musevk;

RfDemodulator::RfDemodulator(Logger &log, std::string executable_dir, std::string filename,
                             musevk::VulkanManager &vulkan_manager, bool benchmark_shaders)
: m_log(log),
  m_executable_dir(std::move(executable_dir)),
  m_filename(std::move(filename)),
  m_vulkan_manager(vulkan_manager),
  m_benchmark_shaders(benchmark_shaders),
  m_input_fd(-1),
  m_input_is_fifo(filesystem::is_fifo(filename)),
  m_total_samples_read(0),
  m_demodulator_thread(nullptr),
  m_vacant_blocks(),
  m_filled_blocks(),
  m_demodulated_block_mutex(),
  m_input_file_mutex(),
  m_cv_filled(),
  m_cv_vacant(),
  m_stop_request(false),
  m_reader_thread_finished(false) {
}

bool RfDemodulator::initialize() {
    m_input_fd = open(m_filename.c_str(), O_NONBLOCK);
    if (m_input_fd == -1)
        throw runtime_error(fmt::format("RfDemodulator: Unable to open input file {}", m_filename));

#ifdef linux
    if (m_input_is_fifo) {
        m_log.debug(eInput, fmt::format("Pipe size: {}", fcntl(m_input_fd, F_GETPIPE_SZ)));
        fcntl(m_input_fd, F_SETPIPE_SZ, 1024 * 1024);
        m_log.debug(eInput, fmt::format("Pipe size now: {}", fcntl(m_input_fd, F_GETPIPE_SZ)));
    }
#endif

    for (int i = 0; i < c_number_of_block_buffers; i++)
        m_vacant_blocks.push_back(make_unique<DemodulatedBlock>(m_vulkan_manager));

    m_demodulator_thread = new thread(&RfDemodulator::demodulate, this);
#ifdef linux
    pthread_setname_np(m_demodulator_thread->native_handle(), "musecpp-demod");
#endif

    return true;
}

std::unique_ptr<DemodulatedBlock> RfDemodulator::getNextDemodulatedBlock() {
    std::unique_lock<std::mutex> lock(m_demodulated_block_mutex);
    m_cv_filled.wait(
            lock,
            [this] { return m_reader_thread_finished || !m_filled_blocks.empty(); });
    if (m_filled_blocks.empty())
        return nullptr;

    auto block = std::move(m_filled_blocks.front());
    m_filled_blocks.pop_front();
    return block;
}

void RfDemodulator::returnBlock(std::unique_ptr<DemodulatedBlock> &buffer) {
    std::unique_lock<std::mutex> lock(m_demodulated_block_mutex);
    m_cv_vacant.notify_one();
    m_vacant_blocks.push_back(std::move(buffer));
}

void RfDemodulator::seek(double seconds) {
    if (!m_input_is_fifo) {
        std::unique_lock<std::mutex> lock(m_input_file_mutex);

        long samples_to_seek = (long)(seconds * c_sample_frequency);
        off_t bytes_to_seek = 2 * samples_to_seek;
        m_log.info(eInput, fmt::format("Seeking relative time {} s, {} samples, {} bytes.",
                                       seconds, samples_to_seek, bytes_to_seek));
        lseek(m_input_fd, bytes_to_seek, SEEK_CUR);
        m_total_samples_read = max(0L, m_total_samples_read + samples_to_seek); // FIXME: locking?

        // Discard any filled buffers
        std::unique_lock<std::mutex> lock2(m_demodulated_block_mutex);
        for (auto &b : m_filled_blocks)
            m_vacant_blocks.push_back(std::move(b));
        m_filled_blocks.clear();
        m_cv_vacant.notify_one();
    }
}

void RfDemodulator::cleanup() {
    {
        std::unique_lock<std::mutex> lock(m_demodulated_block_mutex);
        m_cv_vacant.notify_one();
        m_cv_filled.notify_one();
        m_stop_request = true;
    }
    m_log.debug(eInput, "RfDemodulator: requested stop");
    m_demodulator_thread->join();
    delete m_demodulator_thread;
    close(m_input_fd);
    m_vacant_blocks.clear();
    m_filled_blocks.clear();
}

void RfDemodulator::demodulate() {
    assert(c_sample_block_size % c_video_decimation_rate == 0);
    assert(c_efm_out_buffer_size == DemodulatedBlock::c_efm_block_size);

    CommandPool command_pool(m_vulkan_manager);
    musevk::TimestampQueryPool *timestamp_query_pool =
            m_benchmark_shaders ? new musevk::TimestampQueryPool(m_vulkan_manager.getPhysicalDevice(), m_vulkan_manager.getDevice(), 40) : nullptr;
    musevk::TimestampStatistics timestamp_statistics;
    auto command_buffer = command_pool.createCommandBuffer(timestamp_query_pool);
    if (timestamp_query_pool != nullptr)
        timestamp_query_pool->resetAndSubmit(*command_buffer);

    vk::BufferUsageFlags buffer_usage_flags =
            vk::BufferUsageFlagBits::eStorageBuffer
            | vk::BufferUsageFlagBits::eTransferDst
            | vk::BufferUsageFlagBits::eTransferSrc;

    // Create buffers for all the filters
    shared_ptr<VulkanBuffer> bandpass_filter_re =
            VulkanUtil::createDeviceBuffer(m_vulkan_manager, command_pool, Size(c_bandpass_filter_size), c_bandpass_filter.first);
    shared_ptr<VulkanBuffer> bandpass_filter_im =
            VulkanUtil::createDeviceBuffer(m_vulkan_manager, command_pool, Size(c_bandpass_filter_size), c_bandpass_filter.second);
    shared_ptr<VulkanBuffer> lowpass_filter =
            VulkanUtil::createDeviceBuffer(m_vulkan_manager, command_pool, Size(c_lowpass_filter_size), c_lowpass_filter);
    shared_ptr<VulkanBuffer> rrc_filter =
            VulkanUtil::createDeviceBuffer(m_vulkan_manager, command_pool, Size(c_rrc_filter_size), c_rrc_filter);
    shared_ptr<VulkanBuffer> efm_lowpass_filter =
            VulkanUtil::createDeviceBuffer(m_vulkan_manager, command_pool, Size(c_efm_lowpass_filter_size), c_efm_lowpass_filter);
    shared_ptr<VulkanBuffer> efm_equalization_filter =
            VulkanUtil::createDeviceBuffer(m_vulkan_manager, command_pool, Size(c_efm_equalization_filter_size), c_efm_equalization_filter);

    shared_ptr<VulkanBuffer> input_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(c_input_buffer_size), sizeof(int16_t), buffer_usage_flags, HostAccess::eHostWrite);

    shared_ptr<VulkanBuffer> analytic_buffer_re = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(c_analytic_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> analytic_buffer_im = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(c_analytic_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> lowpass_in_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(c_lowpass_in_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> rrc_in_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(c_rrc_in_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> efm_equalization_in_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(c_efm_equalization_in_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> dropout_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(c_dropout_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    // Create shaders
    shared_ptr<ComputeShader> input_fir_filter_shader = unique_ptr<ComputeShader>(
            new ComputeShader(m_vulkan_manager.getDevice(), "input_fir_filter",
                              {eBuffer, eBuffer, eBuffer}, 4 * sizeof(uint32_t),
                              VulkanUtil::loadSpirv(m_executable_dir, "input_fir_filter.comp"), Size(0), 3));

    input_fir_filter_shader->updateBufferDescriptorsInSet(0, {bandpass_filter_re, input_buffer, analytic_buffer_re});
    input_fir_filter_shader->updateBufferDescriptorsInSet(1, {bandpass_filter_im, input_buffer, analytic_buffer_im});
    input_fir_filter_shader->updateBufferDescriptorsInSet(2, {efm_lowpass_filter, input_buffer, efm_equalization_in_buffer});

    shared_ptr<ComputeShader> fir_filter_shader = unique_ptr<ComputeShader>(
            new ComputeShader(m_vulkan_manager.getDevice(), "fir_filter",
                              {eBuffer, eBuffer, eBuffer}, 4 * sizeof(uint32_t),
                              VulkanUtil::loadSpirv(m_executable_dir, "fir_filter.comp"), Size(0), 3));

    fir_filter_shader->updateBufferDescriptorsInSet(0, {lowpass_filter, lowpass_in_buffer, rrc_in_buffer});

    shared_ptr<ComputeShader> fm_quadrature_shader = unique_ptr<ComputeShader>(
            new ComputeShader(m_vulkan_manager.getDevice(), "fm_quadrature",
                              {analytic_buffer_re, analytic_buffer_im, lowpass_in_buffer}, 7 * sizeof(float),
                              VulkanUtil::loadSpirv(m_executable_dir, "fm_quadrature.comp"), Size(c_sample_block_size)));

    shared_ptr<ComputeShader> detect_dropouts_shader = unique_ptr<ComputeShader>(
            new ComputeShader(m_vulkan_manager.getDevice(),
                              "detect_dropouts",
                              {lowpass_in_buffer, dropout_buffer}, 4 * sizeof(uint32_t),
                              VulkanUtil::loadSpirv(m_executable_dir, "detect_dropouts.comp"), Size(DemodulatedBlock::c_video_block_size)));

    // Clear the buffers -- we start storing data a bit into the buffer, so the first filter pass
    // will have undefined output otherwise.
    command_buffer->begin();
    float zero = 0.f;
    float one = 1.f;
    command_buffer->enqueueFillBuffer(*input_buffer, 0); // int32_t interpreted as 2 x int16_t
    command_buffer->enqueueFillBuffer(*analytic_buffer_re, reinterpret_cast<uint32_t &>(one));
    command_buffer->enqueueFillBuffer(*analytic_buffer_im, reinterpret_cast<uint32_t &>(one));
    command_buffer->enqueueFillBuffer(*lowpass_in_buffer, reinterpret_cast<uint32_t &>(zero));
    command_buffer->enqueueFillBuffer(*rrc_in_buffer, reinterpret_cast<uint32_t &>(zero));
    command_buffer->submit({}, {}, {});
    command_buffer->wait();

    while (!m_stop_request && readFloats(input_buffer->data<int16_t>() + c_bandpass_filter_size - 1, c_sample_block_size)) {

        // First get a free output block to write to
        unique_ptr<DemodulatedBlock> block = nullptr;
        {
            std::unique_lock<std::mutex> lock(m_demodulated_block_mutex);
            if (m_input_is_fifo && m_vacant_blocks.empty()) {
                // discard a filled buffer -- this is better than waiting since it means the reader cannot cope anyway
                m_log.warn(eInput, "Discarding demodulated block due to overrun");
                assert(!m_filled_blocks.empty());
                m_vacant_blocks.push_back(std::move(m_filled_blocks.back()));
                m_filled_blocks.pop_back();
            }
            m_cv_vacant.wait(lock, [this] { return m_stop_request || !m_vacant_blocks.empty(); });
            if (m_stop_request) {
                m_log.info(eInput, "RfDemodulator: stop requested");
                break;
            }
            block = std::move(m_vacant_blocks.front());
            m_vacant_blocks.pop_front();
        }
        // The first byte of the output lags the actual input due to three filters being applied
        block->input_offset = m_total_samples_read - (c_bandpass_filter_size + c_lowpass_filter_size + c_rrc_filter_size) / 2;
        m_total_samples_read += c_sample_block_size;

        // Begin Vulkan command buffer and reset the timestamp query pool if we use one
        if (timestamp_query_pool != nullptr)
            timestamp_query_pool->resetAndSubmit(*command_buffer);
        command_buffer->begin();

        // Run the input signal through the bandpass filter that also converts the signal to an analytic signal
        input_fir_filter_shader->updateWorkgroup(Size(c_sample_block_size));
        command_buffer->enqueueComputeShader<uint32_t>(
                input_fir_filter_shader,
                {c_bandpass_filter_size, c_sample_block_size, /* out offset */ 1, /* decimation */ 1}, 0);
        command_buffer->enqueueComputeShader<uint32_t>(
                input_fir_filter_shader,
                {c_bandpass_filter_size, c_sample_block_size, /* out offset */ 1, /* decimation */ 1}, 1);

        // Demodulate the analytic signal, and scale to the standard MUSE range
        // +/- 1 corresponds to the white/black level, which is 128+/-112 (16, 240) in MUSE
        // With the current parameters (62.5MHz/12.5MHz/1.9MHz) the possible output swing is between -23.03 and +9.87.
        command_buffer->enqueueComputeShader<float>(fm_quadrature_shader,
                                                    {c_sample_block_size, c_lowpass_filter_size - 1,
                                                     c_sample_frequency, c_frequency_deviation, c_center_frequency, /* scale */ 112.f, /* add */ 128.f});

        // Lowpass filter the demodulated signal, and down-sample (decimate by factor 2) before rrc filtering
        fir_filter_shader->updateWorkgroup(Size(c_sample_block_size / c_video_decimation_rate));
        command_buffer->enqueueComputeShader<uint32_t>(
                fir_filter_shader,
                {c_lowpass_filter_size, c_sample_block_size / c_video_decimation_rate, /* out offset */ c_rrc_filter_size - 1, c_video_decimation_rate}, 0);

        // Run the down-sampled signal through the root raised cosine pulse-shaping filter and store in the output block
        fir_filter_shader->updateBufferDescriptorsInSet(1, {rrc_filter, rrc_in_buffer, block->video_data});
        command_buffer->enqueueComputeShader<uint32_t>(
                fir_filter_shader,
                {c_rrc_filter_size, c_sample_block_size / c_video_decimation_rate, /* out offset */ 0, /* decimation */ 1}, 1);

        // EFM
        input_fir_filter_shader->updateWorkgroup(Size(c_efm_out_buffer_size));
        command_buffer->enqueueComputeShader<uint32_t>(
                input_fir_filter_shader,
                {c_rrc_filter_size, c_efm_out_buffer_size, /* out offset */ c_efm_equalization_filter_size - 1, c_efm_decimation_rate}, 2);

        fir_filter_shader->updateBufferDescriptorsInSet(2, {efm_equalization_filter, efm_equalization_in_buffer, block->efm_data});
        command_buffer->enqueueComputeShader<uint32_t>(
                fir_filter_shader,
                {c_efm_equalization_filter_size, c_efm_out_buffer_size, /* out offset */ 0, /* decimation */ 1}, 2);

        // Detect dropouts
        command_buffer->enqueueComputeShader<uint32_t>(
                detect_dropouts_shader, {DemodulatedBlock::c_video_block_size, c_lowpass_filter_size - 1, c_dropout_delay, c_video_decimation_rate});

        // Copy data from dropout detection to the output buffer before writing over the first part of the buffer below
        command_buffer->enqueueCopyBuffer(*dropout_buffer, *block->dropouts, 0, 0, block->c_video_block_size * sizeof(uint8_t));

        // Ensure data can be read from the host later
        block->video_data->synchronizeForHostRead(*command_buffer);
        block->efm_data->synchronizeForHostRead(*command_buffer);
        block->dropouts->synchronizeForHostRead(*command_buffer);

        // After filtering, we need to copy the last data from the input buffers to their start,
        // since the filter output is shorter than the input.  The same is true for the demodulation input,
        // but here only one byte needs to be copied.
        auto enqueue_int16_copy = [command_buffer](VulkanBuffer &buffer, uint32_t buffer_size, uint32_t copy_count) -> void {
            command_buffer->enqueueCopyBuffer(buffer, buffer, (buffer_size - copy_count) * sizeof(int16_t), 0, copy_count * sizeof(int16_t));
        };
        auto enqueue_float_copy = [command_buffer](VulkanBuffer &buffer, uint32_t buffer_size, uint32_t floats_to_copy) -> void {
            command_buffer->enqueueCopyBuffer(buffer, buffer, (buffer_size - floats_to_copy) * sizeof(float), 0, floats_to_copy * sizeof(float));
        };
        enqueue_int16_copy(*input_buffer, c_input_buffer_size, c_bandpass_filter_size - 1);
        enqueue_float_copy(*analytic_buffer_re, c_analytic_buffer_size, 1);
        enqueue_float_copy(*analytic_buffer_im, c_analytic_buffer_size, 1);
        enqueue_float_copy(*lowpass_in_buffer, c_lowpass_in_buffer_size, c_lowpass_filter_size - 1);
        enqueue_float_copy(*rrc_in_buffer, c_rrc_in_buffer_size, c_rrc_filter_size - 1);
        enqueue_float_copy(*efm_equalization_in_buffer, c_efm_equalization_in_buffer_size, c_efm_equalization_filter_size - 1);
        command_buffer->enqueueCopyBuffer(*dropout_buffer, *dropout_buffer, (c_dropout_buffer_size - c_dropout_delay) * sizeof(uint8_t), 0, c_dropout_delay * sizeof(uint8_t));

        command_buffer->submit({}, {}, {});
        command_buffer->wait();

        if (timestamp_query_pool != nullptr)
            timestamp_statistics.add_timestamps(timestamp_query_pool->getTimestamps());

        // Send away result
        std::unique_lock<std::mutex> lock(m_demodulated_block_mutex);
        m_cv_filled.notify_one();
        m_filled_blocks.push_back(std::move(block));
    }
    m_reader_thread_finished = true;
    m_cv_filled.notify_one();

    delete timestamp_query_pool;
    timestamp_statistics.print_stats(0);
}

bool RfDemodulator::readFloats(int16_t *out, size_t n) {
    int filled_bytes = 0;
    do {
        if (m_stop_request) {
            m_log.info(eInput, "RfDemodulator: stop requested");
            return false;
        }
        std::scoped_lock<std::mutex> lock(m_input_file_mutex);
        ssize_t read_count = read(m_input_fd,
                                  (void *)((char *)out + filled_bytes),
                                  n * sizeof(int16_t) - filled_bytes);
        if (read_count == -1 && errno == EAGAIN)
            this_thread::sleep_for(chrono::milliseconds(1));
        else if (read_count == 0) {
            if (!m_input_is_fifo) {
                m_log.info(eInput, "RfDemodulator: end of file");
                return false;
            }
        } else if (read_count == -1)
            throw runtime_error(fmt::format("Error reading from file: {}", strerror(errno)));
        else
            filled_bytes += (int)read_count;
    } while (filled_bytes < n * sizeof(int16_t));

    return true;
}
