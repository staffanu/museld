//
// Created by staffanu on 12/10/23.
//

#include <thread>
#include <utility>
#include <vector>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include "NtscRfDemodulator.h"
#include "filter/WindowedSinc.h"
#include "musevk/VulkanUtil.h"
#include "musevk/TimestampQueryPool.h"
#include "musevk/TimestampStatistics.h"

using namespace std;
using namespace musevk;
using namespace NtscRfDemodulatorConstants;

NtscRfDemodulator::NtscRfDemodulator(Logger &log, std::string executable_dir, std::string filename,  float sample_frequency,
                                     musevk::VulkanManager &vulkan_manager, InputFormat input_format, bool benchmark_shaders,
                                     bool efm_enabled)
: RfDemodulator<NtscDemodulatedBlock>(log, std::move(executable_dir), std::move(filename), sample_frequency,
                                      vulkan_manager, input_format,
                                      NtscRfDemodulatorConstants::c_sample_block_size,
                                      benchmark_shaders),
  m_efm_demodulator(log, sample_frequency, NtscRfDemodulatorConstants::c_sample_block_size, true, true, 2, 3, std::nullopt),
  m_efm_enabled(efm_enabled) {
}

void NtscRfDemodulator::demodulate() {
    assert(c_sample_block_size % c_video_decimation_rate == 0);

    CommandPool command_pool(m_vulkan_manager);
    musevk::TimestampQueryPool *timestamp_query_pool =
            m_benchmark_shaders ? new musevk::TimestampQueryPool(m_vulkan_manager.getPhysicalDevice(), m_vulkan_manager.getDevice(), 40) : nullptr;
    musevk::TimestampStatistics timestamp_statistics;
    auto command_buffer = command_pool.createCommandBuffer(timestamp_query_pool);

    vk::BufferUsageFlags buffer_usage_flags =
            vk::BufferUsageFlagBits::eStorageBuffer
            | vk::BufferUsageFlagBits::eTransferDst
            | vk::BufferUsageFlagBits::eTransferSrc;


    // Create all the filters

    // FIR band-pass filter that also creates an analytic signal.  The passband is 3.5 to 13.5 MHz.
    // Reverse because the FIR shader correlates rather than convolves.
    std::vector<std::complex<float>> bandpass_filter_def =
            WindowedSinc::complex_band_pass<float>(WindowedSinc::rectangular_ntaps(40e6, 1.5e6),
                                                   40.0e6, 3.5e6, 13.5e6);
    std::reverse(bandpass_filter_def.begin(), bandpass_filter_def.end());

    std::vector<float> bandpass_filter_re_coeffs;
    std::transform(bandpass_filter_def.cbegin(), bandpass_filter_def.cend(), back_inserter(bandpass_filter_re_coeffs),
                   [](std::complex<float> c) { return c.real(); });
    std::vector<float> bandpass_filter_im_coeffs;
    std::transform(bandpass_filter_def.cbegin(), bandpass_filter_def.cend(), back_inserter(bandpass_filter_im_coeffs),
                   [](std::complex<float> c) { return c.imag(); });

    shared_ptr<VulkanBuffer> bandpass_filter_re =
            VulkanUtil::createDeviceBuffer(m_vulkan_manager, command_pool, Size(bandpass_filter_def.size()), bandpass_filter_re_coeffs);
    shared_ptr<VulkanBuffer> bandpass_filter_im =
            VulkanUtil::createDeviceBuffer(m_vulkan_manager, command_pool, Size(bandpass_filter_def.size()), bandpass_filter_im_coeffs);

    // FIR lowpass filter for the demodulated signal
    std::vector<float> lowpass_filter_def =
            WindowedSinc::low_pass<float>(WindowedSinc::rectangular_ntaps(40e6, 2e6), 40e6, 5e6);
    std::reverse(lowpass_filter_def.begin(), lowpass_filter_def.end()); // symmetric, but the shader correlates

    shared_ptr<VulkanBuffer> lowpass_filter =
            VulkanUtil::createDeviceBuffer(m_vulkan_manager, command_pool, Size(lowpass_filter_def.size()), lowpass_filter_def);

    // FIR de-emphasis filter (applied to the decimated lowpass filtered signal)
    // For NTSC, the two frequencies are 3.125 MHz and 8.33 MHz
    std::vector<float> deemphasis_filter_def =
            WindowedSinc::low_pass<float>(WindowedSinc::rectangular_ntaps(20e6, 5e6), 20e6, 5e6);
    std::reverse(deemphasis_filter_def.begin(), deemphasis_filter_def.end()); // symmetric, but the shader correlates

    shared_ptr<VulkanBuffer> deemphasis_filter =
            VulkanUtil::createDeviceBuffer(m_vulkan_manager, command_pool, Size(deemphasis_filter_def.size()), deemphasis_filter_def);

    // Create buffers for data
    const int input_buffer_size = c_sample_block_size + (int)bandpass_filter_def.size() - 1;
    const int analytic_buffer_size = c_sample_block_size + 1;
    const int lowpass_in_buffer_size = c_sample_block_size + (int)lowpass_filter_def.size() - 1;
    const int deemphasis_in_buffer_size = c_sample_block_size + (int)deemphasis_filter_def.size() - 1;

    // We need to delay the output of the detected dropouts as much as the rest of the filter chain delays the video signal
    const int dropout_delay = (int)lowpass_filter_def.size() / c_video_decimation_rate / 2 - 1;
    const int dropout_buffer_size = c_video_block_size + dropout_delay;

    shared_ptr<VulkanBuffer> input_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(input_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostWrite);

    shared_ptr<VulkanBuffer> analytic_buffer_re = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(analytic_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> analytic_buffer_im = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(analytic_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> lowpass_in_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(lowpass_in_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> equalization_in_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(deemphasis_in_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> dropout_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(dropout_buffer_size), sizeof(uint8_t), buffer_usage_flags, HostAccess::eHostNone);


    // Create shaders
    shared_ptr<ComputeShader> input_fir_filter_shader = unique_ptr<ComputeShader>(
            new ComputeShader(m_vulkan_manager.getDevice(), "input_fir_filter",
                              {eBuffer, eBuffer, eBuffer}, 4 * sizeof(uint32_t),
                              VulkanUtil::loadSpirv(m_executable_dir, "input_fir_filter.comp"), Size(0), 2));

    input_fir_filter_shader->updateBufferDescriptorsInSet(0, {bandpass_filter_re, input_buffer, analytic_buffer_re});
    input_fir_filter_shader->updateBufferDescriptorsInSet(1, {bandpass_filter_im, input_buffer, analytic_buffer_im});

    shared_ptr<ComputeShader> fir_filter_shader = unique_ptr<ComputeShader>(
            new ComputeShader(m_vulkan_manager.getDevice(), "fir_filter",
                              {eBuffer, eBuffer, eBuffer}, 4 * sizeof(uint32_t),
                              VulkanUtil::loadSpirv(m_executable_dir, "fir_filter.comp"), Size(0), 2));

    fir_filter_shader->updateBufferDescriptorsInSet(0, {lowpass_filter, lowpass_in_buffer, equalization_in_buffer});

    shared_ptr<ComputeShader> fm_quadrature_shader = unique_ptr<ComputeShader>(
            new ComputeShader(m_vulkan_manager.getDevice(), "fm_quadrature",
                              {analytic_buffer_re, analytic_buffer_im, lowpass_in_buffer}, 7 * sizeof(float),
                              VulkanUtil::loadSpirv(m_executable_dir, "fm_quadrature.comp"), Size(c_sample_block_size)));

    shared_ptr<ComputeShader> detect_dropouts_shader = unique_ptr<ComputeShader>(
            new ComputeShader(m_vulkan_manager.getDevice(),
                              "detect_dropouts",
                              {lowpass_in_buffer, dropout_buffer}, 4 * sizeof(uint32_t),
                              VulkanUtil::loadSpirv(m_executable_dir, "detect_dropouts.comp"), Size(NtscRfDemodulatorConstants::c_video_block_size)));

    // Clear the buffers -- we start storing data a bit into the buffer, so the first filter pass
    // will have undefined output otherwise.
    command_buffer->begin();
    if (timestamp_query_pool != nullptr)
        timestamp_query_pool->reset(*command_buffer);
    float zero = 0.f;
    float one = 1.f;
    command_buffer->enqueueFillBuffer(*input_buffer, 0);
    command_buffer->enqueueFillBuffer(*analytic_buffer_re, reinterpret_cast<uint32_t &>(one));
    command_buffer->enqueueFillBuffer(*analytic_buffer_im, reinterpret_cast<uint32_t &>(one));
    command_buffer->enqueueFillBuffer(*lowpass_in_buffer, reinterpret_cast<uint32_t &>(zero));
    command_buffer->submit({}, {}, {});
    command_buffer->wait();

    while (!m_stop_request && readFloats(input_buffer->data<float>() + bandpass_filter_def.size() - 1, c_sample_block_size)) {

        // First get a free output block to write to
        unique_ptr<NtscDemodulatedBlock> block = nullptr;
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
                m_log.info(eInput, "NtscRfDemodulator: stop requested");
                break;
            }
            block = std::move(m_vacant_blocks.front());
            m_vacant_blocks.pop_front();
        }
        // The first byte of the output lags the actual input due to three filters being applied
        block->input_offset = m_total_samples_read - (bandpass_filter_def.size() + lowpass_filter_def.size()) / 2;
        m_total_samples_read += c_sample_block_size;

        // Begin Vulkan command buffer and reset the timestamp query pool if we use one
        command_buffer->begin();
        if (timestamp_query_pool != nullptr)
            timestamp_query_pool->reset(*command_buffer);

        // Make the data available on the GPU; this is necessary in case we didn't find a memory type good for both host writes and shader reads
        input_buffer->synchronizeHostWrites(*command_buffer);

        // Run the input signal through the bandpass filter that also converts the signal to an analytic signal
        input_fir_filter_shader->updateWorkgroup(Size(c_sample_block_size));
        command_buffer->enqueueComputeShader<uint32_t>(
                input_fir_filter_shader,
                {(uint32_t)bandpass_filter_def.size(), c_sample_block_size, /* out offset */ 1, /* decimation */ 1}, 0);
        command_buffer->enqueueComputeShader<uint32_t>(
                input_fir_filter_shader,
                {(uint32_t)bandpass_filter_def.size(), c_sample_block_size, /* out offset */ 1, /* decimation */ 1}, 1);

        // Demodulate the analytic signal, and scale to [0, 1].
        command_buffer->enqueueComputeShader<float>(fm_quadrature_shader,
                                                    {c_sample_block_size, (float)lowpass_filter_def.size() - 1,
                                                     c_sample_frequency, c_frequency_deviation, c_center_frequency, /* scale */ 0.5f, /* add */ 0.5f});

        // Lowpass filter the demodulated signal, and down-sample (decimate by factor 2)
        fir_filter_shader->updateWorkgroup(Size(c_video_block_size));
        command_buffer->enqueueComputeShader<uint32_t>(
                fir_filter_shader,
                {(uint32_t)lowpass_filter_def.size(), c_video_block_size, /* out offset */ 0, c_video_decimation_rate}, 0);

        // Run the down-sampled signal through the de-emphasis filter and store in the output block
        fir_filter_shader->updateBufferDescriptorsInSet(1, {deemphasis_filter, equalization_in_buffer, block->video_data});
        command_buffer->enqueueComputeShader<uint32_t>(
                fir_filter_shader,
                {(uint32_t)deemphasis_filter_def.size(), c_video_block_size, /* out offset */ 0, /* decimation */ 1}, 1);

        // Detect dropouts FIXME update for LD
        command_buffer->enqueueComputeShader<uint32_t>(
                detect_dropouts_shader, {NtscRfDemodulatorConstants::c_video_block_size, (uint32_t)lowpass_filter_def.size() - 1, (uint32_t)dropout_delay, c_video_decimation_rate});

        // Barrier: ensure all compute shader writes are visible to the subsequent transfer operations
        command_buffer->enqueueBarrier(vk::AccessFlagBits::eShaderWrite,
                                       vk::AccessFlagBits::eTransferRead,
                                       vk::PipelineStageFlagBits::eComputeShader,
                                       vk::PipelineStageFlagBits::eTransfer);

        // Copy data from dropout detection to the output buffer before writing over the first part of the buffer below
        command_buffer->enqueueCopyBuffer(*dropout_buffer, *block->dropouts, 0, 0, NtscRfDemodulatorConstants::c_video_block_size * sizeof(uint8_t));

        // Ensure data can be read from the host later
        block->video_data->synchronizeForHostRead(*command_buffer);
        block->dropouts->synchronizeForHostRead(*command_buffer);

        // After filtering, we need to copy the last data from the input buffers to their start,
        // since the filter output is shorter than the input.  The same is true for the demodulation input,
        // but here only one byte needs to be copied.
        auto enqueue_float_copy = [command_buffer](VulkanBuffer &buffer, uint32_t buffer_size, uint32_t floats_to_copy) -> void {
            command_buffer->enqueueCopyBuffer(buffer, buffer, (buffer_size - floats_to_copy) * sizeof(float), 0, floats_to_copy * sizeof(float));
        };
        enqueue_float_copy(*input_buffer, input_buffer_size, bandpass_filter_def.size() - 1);
        enqueue_float_copy(*analytic_buffer_re, analytic_buffer_size, 1);
        enqueue_float_copy(*analytic_buffer_im, analytic_buffer_size, 1);
        enqueue_float_copy(*lowpass_in_buffer, lowpass_in_buffer_size, lowpass_filter_def.size() - 1);
        command_buffer->enqueueCopyBuffer(*dropout_buffer, *dropout_buffer, (dropout_buffer_size - dropout_delay) * sizeof(uint8_t), 0, dropout_delay * sizeof(uint8_t));

        command_buffer->submit({}, {}, {});

        if (m_efm_enabled)
            m_efm_demodulator.demodulate(input_buffer->data<float>() + bandpass_filter_def.size() - 1, block->efm_data);
        else
            block->efm_data.clear();

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
