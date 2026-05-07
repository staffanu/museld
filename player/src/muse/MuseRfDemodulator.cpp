//
// Created by staffanu on 12/10/23.
//

#include <thread>
#include <utility>
#include <vector>
#include <iostream>
#include <filesystem>
#include <gnuradio/fft/window.h>
#include <gnuradio/filter/firdes.h>
#include "MuseRfDemodulator.h"
#include "musevk/VulkanUtil.h"
#include "musevk/TimestampQueryPool.h"
#include "musevk/TimestampStatistics.h"

using namespace std;
using namespace musevk;

MuseRfDemodulator::MuseRfDemodulator(Logger &log, std::string executable_dir, std::string filename,
                                     float sample_frequency,
                                     musevk::VulkanManager &vulkan_manager, InputFormat input_format, bool benchmark_shaders,
                                     bool efm_enabled)
: RfDemodulator<MuseDemodulatedBlock>(log, std::move(executable_dir), std::move(filename), sample_frequency,
                                      vulkan_manager, input_format, MuseDemodulatedBlock::c_sample_block_size,
                                      benchmark_shaders),
  m_efm_demodulator(log, sample_frequency, MuseDemodulatedBlock::c_sample_block_size, true, true, 2, 3, std::nullopt),
  m_efm_enabled(efm_enabled) {
}

void MuseRfDemodulator::demodulate() {
    CommandPool command_pool(m_vulkan_manager);
    musevk::TimestampQueryPool *timestamp_query_pool =
            m_benchmark_shaders ? new musevk::TimestampQueryPool(m_vulkan_manager.getPhysicalDevice(), m_vulkan_manager.getDevice(), 40) : nullptr;
    musevk::TimestampStatistics timestamp_statistics;
    auto command_buffer = command_pool.createCommandBuffer(timestamp_query_pool);

    vk::BufferUsageFlags buffer_usage_flags =
            vk::BufferUsageFlagBits::eStorageBuffer
            | vk::BufferUsageFlagBits::eTransferDst
            | vk::BufferUsageFlagBits::eTransferSrc;

    // Create buffers for all the filters

    // FIR band-pass filter to filter out noise over 22.5 MHz and also the pilot signal and EFM.
    // The pilot signal isn't completely suppressed but that doesn't seem to matter.
    std::vector<std::complex<float>> bandpass_filter_def =
            gr::filter::firdes::complex_band_pass(1.0, m_sample_frequency, 2.7e6, 22.3e6, 2.0e6, gr::fft::window::WIN_RECTANGULAR);
    std::reverse(bandpass_filter_def.begin(), bandpass_filter_def.end());
    const int bandpass_filter_size = (int)bandpass_filter_def.size();

    std::vector<float> bandpass_filter_re_coeffs;
    std::transform(bandpass_filter_def.cbegin(), bandpass_filter_def.cend(), back_inserter(bandpass_filter_re_coeffs),
                   [](std::complex<float> c) { return c.real(); });
    std::vector<float> bandpass_filter_im_coeffs;
    std::transform(bandpass_filter_def.cbegin(), bandpass_filter_def.cend(), back_inserter(bandpass_filter_im_coeffs),
                   [](std::complex<float> c) { return c.imag(); });

    shared_ptr<VulkanBuffer> bandpass_filter_re =
            VulkanUtil::createDeviceBuffer(m_vulkan_manager, command_pool, Size(bandpass_filter_size), bandpass_filter_re_coeffs);
    shared_ptr<VulkanBuffer> bandpass_filter_im =
            VulkanUtil::createDeviceBuffer(m_vulkan_manager, command_pool, Size(bandpass_filter_size), bandpass_filter_im_coeffs);

    // FIR lowpass filter for the demodulated signal
    std::vector<float> lowpass_filter_def =
            gr::filter::firdes::low_pass(1.0, m_sample_frequency, 9.8e6, 2e6, gr::fft::window::WIN_RECTANGULAR);
    std::reverse(lowpass_filter_def.begin(), lowpass_filter_def.end()); // should be symmetric, so really unnecessary
    const int lowpass_filter_size = (int)lowpass_filter_def.size();

    shared_ptr<VulkanBuffer> lowpass_filter =
            VulkanUtil::createDeviceBuffer(m_vulkan_manager, command_pool, Size(lowpass_filter_size), lowpass_filter_def);

    // Root raised cosine filter for pulse shaping
    // Notice that the root raised cosine filter works on half the input sample rate, i.e., 31.25 MHz
    std::vector<float> rrc_filter_def =
            gr::filter::firdes::root_raised_cosine(1.0, m_sample_frequency / 2, 16.2e6, 0.1, 11);
    std::reverse(rrc_filter_def.begin(), rrc_filter_def.end()); // should be symmetric, so really unnecessary
    const int rrc_filter_size = (int)rrc_filter_def.size();

    shared_ptr<VulkanBuffer> rrc_filter =
            VulkanUtil::createDeviceBuffer(m_vulkan_manager, command_pool, Size(rrc_filter_size), rrc_filter_def);

    // Create buffers for data
    const int input_buffer_size = MuseDemodulatedBlock::c_sample_block_size + bandpass_filter_size - 1;
    const int analytic_buffer_size = MuseDemodulatedBlock::c_sample_block_size + 1;
    const int lowpass_in_buffer_size = MuseDemodulatedBlock::c_sample_block_size + lowpass_filter_size - 1;
    const int rrc_in_buffer_size = MuseDemodulatedBlock::c_video_block_size + rrc_filter_size - 1;

    // We need to delay the output of the detected dropouts as much as the rest of the filter chain delays the video signal
    const int c_dropout_delay = lowpass_filter_size / MuseDemodulatedBlock::c_video_decimation_rate / 2 + rrc_filter_size / 2 - 1;
    const int c_dropout_buffer_size = MuseDemodulatedBlock::c_video_block_size + c_dropout_delay;

    shared_ptr<VulkanBuffer> input_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(input_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostWrite);

    shared_ptr<VulkanBuffer> analytic_buffer_re = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(analytic_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> analytic_buffer_im = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(analytic_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> lowpass_in_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(lowpass_in_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> rrc_in_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(rrc_in_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> dropout_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(c_dropout_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

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

    fir_filter_shader->updateBufferDescriptorsInSet(0, {lowpass_filter, lowpass_in_buffer, rrc_in_buffer});

    shared_ptr<ComputeShader> fm_quadrature_shader = unique_ptr<ComputeShader>(
            new ComputeShader(m_vulkan_manager.getDevice(), "fm_quadrature",
                              {analytic_buffer_re, analytic_buffer_im, lowpass_in_buffer}, 7 * sizeof(float),
                              VulkanUtil::loadSpirv(m_executable_dir, "fm_quadrature.comp"), Size(MuseDemodulatedBlock::c_sample_block_size)));

    shared_ptr<ComputeShader> detect_dropouts_shader = unique_ptr<ComputeShader>(
            new ComputeShader(m_vulkan_manager.getDevice(),
                              "detect_dropouts",
                              {lowpass_in_buffer, dropout_buffer}, 4 * sizeof(uint32_t),
                              VulkanUtil::loadSpirv(m_executable_dir, "detect_dropouts.comp"), Size(MuseDemodulatedBlock::c_video_block_size)));

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
    command_buffer->enqueueFillBuffer(*rrc_in_buffer, reinterpret_cast<uint32_t &>(zero));
    command_buffer->submit({}, {}, {});
    command_buffer->wait();

    while (!m_stop_request && readFloats(input_buffer->data<float>() + bandpass_filter_size - 1, MuseDemodulatedBlock::c_sample_block_size)) {

        // First get a free output block to write to
        unique_ptr<MuseDemodulatedBlock> block = nullptr;
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
                m_log.info(eInput, "MuseRfDemodulator: stop requested");
                break;
            }
            block = std::move(m_vacant_blocks.front());
            m_vacant_blocks.pop_front();
        }
        // The first byte of the output lags the actual input due to three filters being applied
        block->input_offset = m_total_samples_read - (bandpass_filter_size + lowpass_filter_size + rrc_filter_size) / 2;
        m_total_samples_read += MuseDemodulatedBlock::c_sample_block_size;

        // Begin Vulkan command buffer and reset the timestamp query pool if we use one
        command_buffer->begin();
        if (timestamp_query_pool != nullptr)
            timestamp_query_pool->reset(*command_buffer);

        // Make the data available on the GPU; this is necessary in case we didn't find a memory type good for both host writes and shader reads
        input_buffer->synchronizeHostWrites(*command_buffer);

        // Run the input signal through the bandpass filter that also converts the signal to an analytic signal
        input_fir_filter_shader->updateWorkgroup(Size(MuseDemodulatedBlock::c_sample_block_size));
        command_buffer->enqueueComputeShader<uint32_t>(
                input_fir_filter_shader,
                {(uint32_t)bandpass_filter_size, MuseDemodulatedBlock::c_sample_block_size, /* out offset */ 1, /* decimation */ 1}, 0);
        command_buffer->enqueueComputeShader<uint32_t>(
                input_fir_filter_shader,
                {(uint32_t)bandpass_filter_size, MuseDemodulatedBlock::c_sample_block_size, /* out offset */ 1, /* decimation */ 1}, 1);

        // Demodulate the analytic signal, and scale to the standard MUSE range
        // +/- 1 corresponds to the white/black level, which is 128+/-112 (16, 240) in MUSE
        // With the current parameters (62.5MHz/12.5MHz/1.9MHz) the possible output swing is between -23.03 and +9.87.
        command_buffer->enqueueComputeShader<float>(fm_quadrature_shader,
                                                    {MuseDemodulatedBlock::c_sample_block_size, (float)lowpass_filter_size - 1,
                                                     m_sample_frequency, c_frequency_deviation, c_center_frequency, /* scale */ 112.f, /* add */ 128.f});

        // Lowpass filter the demodulated signal, and down-sample (decimate by factor 2) before rrc filtering
        fir_filter_shader->updateWorkgroup(Size(MuseDemodulatedBlock::c_video_block_size));
        command_buffer->enqueueComputeShader<uint32_t>(
                fir_filter_shader,
                {(uint32_t)lowpass_filter_size, MuseDemodulatedBlock::c_video_block_size,
                 /* out offset */ (uint32_t)rrc_filter_size - 1, MuseDemodulatedBlock::c_video_decimation_rate}, 0);

        // Run the down-sampled signal through the root raised cosine pulse-shaping filter and store in the output block
        fir_filter_shader->updateBufferDescriptorsInSet(1, {rrc_filter, rrc_in_buffer, block->video_data});
        command_buffer->enqueueComputeShader<uint32_t>(
                fir_filter_shader,
                {(uint32_t)rrc_filter_size, MuseDemodulatedBlock::c_video_block_size,
                 /* out offset */ 0, /* decimation */ 1}, 1);

        // Detect dropouts
        command_buffer->enqueueComputeShader<uint32_t>(
                detect_dropouts_shader, {MuseDemodulatedBlock::c_video_block_size,
                                         (uint32_t)lowpass_filter_size - 1, (uint32_t)c_dropout_delay, MuseDemodulatedBlock::c_video_decimation_rate});

        // Copy data from dropout detection to the output buffer before writing over the first part of the buffer below
        command_buffer->enqueueCopyBuffer(*dropout_buffer, *block->dropouts, 0, 0, MuseDemodulatedBlock::c_video_block_size * sizeof(uint8_t));

        // Ensure data can be read from the host later
        block->video_data->synchronizeForHostRead(*command_buffer);
        block->dropouts->synchronizeForHostRead(*command_buffer);

        // After filtering, we need to copy the last data from the input buffers to their start,
        // since the filter output is shorter than the input.  The same is true for the demodulation input,
        // but here only one byte needs to be copied.
        auto enqueue_float_copy = [command_buffer](VulkanBuffer &buffer, uint32_t buffer_size, uint32_t floats_to_copy) -> void {
            command_buffer->enqueueCopyBuffer(buffer, buffer, (buffer_size - floats_to_copy) * sizeof(float), 0, floats_to_copy * sizeof(float));
        };
        enqueue_float_copy(*input_buffer, input_buffer_size, bandpass_filter_size - 1);
        enqueue_float_copy(*analytic_buffer_re, analytic_buffer_size, 1);
        enqueue_float_copy(*analytic_buffer_im, analytic_buffer_size, 1);
        enqueue_float_copy(*lowpass_in_buffer, lowpass_in_buffer_size, lowpass_filter_size - 1);
        enqueue_float_copy(*rrc_in_buffer, rrc_in_buffer_size, rrc_filter_size - 1);
        command_buffer->enqueueCopyBuffer(*dropout_buffer, *dropout_buffer, (c_dropout_buffer_size - c_dropout_delay) * sizeof(uint8_t), 0, c_dropout_delay * sizeof(uint8_t));

        command_buffer->submit({}, {}, {});

        if (m_efm_enabled)
            m_efm_demodulator.demodulate(input_buffer->data<float>() + bandpass_filter_size - 1, block->efm_data);
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
