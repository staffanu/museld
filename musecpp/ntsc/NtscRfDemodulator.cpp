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
#include "musevk/VulkanUtil.h"
#include "musevk/TimestampQueryPool.h"
#include "musevk/TimestampStatistics.h"

using namespace std;
using namespace musevk;
using namespace NtscRfDemodulatorConstants;

NtscRfDemodulator::NtscRfDemodulator(Logger &log, std::string filename, std::string executable_dir,
                                     musevk::VulkanManager &vulkan_manager, bool benchmark_shaders)
: RfDemodulator<NtscDemodulatedBlock>(log, std::move(filename), std::move(executable_dir), vulkan_manager, benchmark_shaders,
                                      NtscRfDemodulatorConstants::c_sample_frequency) {
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
    std::vector<std::complex<float>> bandpass_filter_def =
            gr::filter::firdes::complex_band_pass(1.0, 40.0e6, 3.5e6, 13.5e6, 1.5e6, gr::fft::window::WIN_RECTANGULAR);
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
            gr::filter::firdes::low_pass(1.0, 40e6, 5.0e6, 2e6, gr::fft::window::WIN_RECTANGULAR);
    std::reverse(lowpass_filter_def.begin(), lowpass_filter_def.end()); // should be symmetric, so really unnecessary

    shared_ptr<VulkanBuffer> lowpass_filter =
            VulkanUtil::createDeviceBuffer(m_vulkan_manager, command_pool, Size(lowpass_filter_def.size()), lowpass_filter_def);

    // FIR de-emphasis filter (applied to the decimated lowpass filtered signal)
    // For NTSC, the two frequencies are 3.125 MHz and 8.33 MHz
    std::vector<float> deemphasis_filter_def =
            gr::filter::firdes::low_pass(1.0, 20e6, 1e6, 5e6, gr::fft::window::WIN_RECTANGULAR);
    std::reverse(deemphasis_filter_def.begin(), deemphasis_filter_def.end());

    shared_ptr<VulkanBuffer> deemphasis_filter =
            VulkanUtil::createDeviceBuffer(m_vulkan_manager, command_pool, Size(deemphasis_filter_def.size()), deemphasis_filter_def);

    // Lowpass filter for EFM
    auto efm_lowpass_filter_def = std::vector<float> {
            // Computed in octave: fir1(15, 2.0e6 / 20e6)
            0.003511, 0.007547, 0.019027, 0.039655, 0.067765, 0.098366, 0.124521, 0.139610,
            0.139610, 0.124521, 0.098366, 0.067765, 0.039655, 0.019027, 0.007547, 0.003511
    };
    shared_ptr<VulkanBuffer> efm_lowpass_filter =
            VulkanUtil::createDeviceBuffer(m_vulkan_manager, command_pool, Size(efm_lowpass_filter_def.size()), efm_lowpass_filter_def);

    // Equalization filter for EFM.  Works on 1/4 of the original sample frequency
    // Computed in octave using the script make_efm_filter.m, and then numerically
    // optimized using nonlin_min(samin), for decimation factor 4, lowpass 2.0 MHz
    auto efm_equalization_filter_def = std::vector<float> { // FIXME: recompute for LD
            -0.037621, -0.032380, -0.152833, 0.016591, -0.181817, -0.042276, -0.011685, 0.017400,
            0.112888, 0.095248, 0.226369, 0.508954, 0.470778, 0.771520, 0.435198, 0.410012,
            -0.035066, -0.222377, -0.554291, -0.691465, -0.670789, -0.418965, -0.299890, 0.022324,
            0.139150, 0.209963, 0.198224, 0.001872,-0.033351, 0.040845, -0.023787, 0.024088
    };
    std::reverse(efm_equalization_filter_def.begin(), efm_equalization_filter_def.end());

    shared_ptr<VulkanBuffer> efm_equalization_filter =
            VulkanUtil::createDeviceBuffer(m_vulkan_manager, command_pool, Size(efm_equalization_filter_def.size()), efm_equalization_filter_def);


    // Create buffers for data
    const int input_buffer_size = c_sample_block_size + (int)bandpass_filter_def.size() - 1;
    const int analytic_buffer_size = c_sample_block_size + 1;
    const int lowpass_in_buffer_size = c_sample_block_size + (int)lowpass_filter_def.size() - 1;
    const int deemphasis_in_buffer_size = c_sample_block_size + (int)deemphasis_filter_def.size() - 1;

    // We need to delay the output of the detected dropouts as much as the rest of the filter chain delays the video signal
    const int dropout_delay = (int)lowpass_filter_def.size() / c_video_decimation_rate / 2 - 1;
    const int dropout_buffer_size = c_video_block_size + dropout_delay;

    const int efm_equalization_in_buffer_size = c_sample_block_size / c_efm_decimation_rate + (int)efm_equalization_filter_def.size() - 1;

    shared_ptr<VulkanBuffer> input_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(input_buffer_size), sizeof(int16_t), buffer_usage_flags, HostAccess::eHostWrite);

    shared_ptr<VulkanBuffer> analytic_buffer_re = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(analytic_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> analytic_buffer_im = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(analytic_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> lowpass_in_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(lowpass_in_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> equalization_in_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(deemphasis_in_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> efm_equalization_in_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(efm_equalization_in_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> dropout_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(dropout_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);


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
    command_buffer->enqueueFillBuffer(*input_buffer, 0); // int32_t interpreted as 2 x int16_t
    command_buffer->enqueueFillBuffer(*analytic_buffer_re, reinterpret_cast<uint32_t &>(one));
    command_buffer->enqueueFillBuffer(*analytic_buffer_im, reinterpret_cast<uint32_t &>(one));
    command_buffer->enqueueFillBuffer(*lowpass_in_buffer, reinterpret_cast<uint32_t &>(zero));
    command_buffer->submit({}, {}, {});
    command_buffer->wait();

    while (!m_stop_request && readFloats(input_buffer->data<int16_t>() + bandpass_filter_def.size() - 1, c_sample_block_size)) {

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
                m_log.info(eInput, "MuseRfDemodulator: stop requested");
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

        // Demodulate the analytic signal, and scale to the standard MUSE range
        // +/- 1 corresponds to the white/sync level, which we scale to [0, 1].
        // With 40 MHz sample rate, 8.5 MHz center, 0.85 MHz deviation, the possible output swing is between ???.
        command_buffer->enqueueComputeShader<float>(fm_quadrature_shader,
                                                    {c_sample_block_size, (float)lowpass_filter_def.size() - 1,
                                                     c_sample_frequency, c_frequency_deviation, c_center_frequency, /* scale */ 0.5f, /* add */ 0.5f});

        // Lowpass filter the demodulated signal, and down-sample (decimate by factor 2)
        fir_filter_shader->updateWorkgroup(Size(c_sample_block_size / c_video_decimation_rate));
        command_buffer->enqueueComputeShader<uint32_t>(
                fir_filter_shader,
                {(uint32_t)lowpass_filter_def.size(), c_sample_block_size / c_video_decimation_rate, /* out offset */ 0, c_video_decimation_rate}, 0);

        // Run the down-sampled signal through the de-emphasis filter and store in the output block
        fir_filter_shader->updateBufferDescriptorsInSet(1, {deemphasis_filter, equalization_in_buffer, block->video_data});
        command_buffer->enqueueComputeShader<uint32_t>(
                fir_filter_shader,
                {(uint32_t)deemphasis_filter_def.size(), c_sample_block_size / c_video_decimation_rate, /* out offset */ 0, /* decimation */ 1}, 1);

        // EFM
        input_fir_filter_shader->updateWorkgroup(Size(NtscRfDemodulatorConstants::c_efm_block_size));
        command_buffer->enqueueComputeShader<uint32_t>(
                input_fir_filter_shader,
                {(uint32_t)efm_lowpass_filter_def.size(), NtscRfDemodulatorConstants::c_efm_block_size, /* out offset */ (uint32_t)efm_equalization_filter_def.size() - 1, c_efm_decimation_rate}, 2);

        fir_filter_shader->updateBufferDescriptorsInSet(2, {efm_equalization_filter, efm_equalization_in_buffer, block->efm_data});
        command_buffer->enqueueComputeShader<uint32_t>(
                fir_filter_shader,
                {(uint32_t)efm_equalization_filter_def.size(), NtscRfDemodulatorConstants::c_efm_block_size, /* out offset */ 0, /* decimation */ 1}, 2);

        // Detect dropouts FIXME update for LD
        command_buffer->enqueueComputeShader<uint32_t>(
                detect_dropouts_shader, {NtscRfDemodulatorConstants::c_video_block_size, (uint32_t)lowpass_filter_def.size() - 1, (uint32_t)dropout_delay, c_video_decimation_rate});

        // Copy data from dropout detection to the output buffer before writing over the first part of the buffer below
        command_buffer->enqueueCopyBuffer(*dropout_buffer, *block->dropouts, 0, 0, NtscRfDemodulatorConstants::c_video_block_size * sizeof(uint8_t));

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
        enqueue_int16_copy(*input_buffer, input_buffer_size, bandpass_filter_def.size() - 1);
        enqueue_float_copy(*analytic_buffer_re, analytic_buffer_size, 1);
        enqueue_float_copy(*analytic_buffer_im, analytic_buffer_size, 1);
        enqueue_float_copy(*lowpass_in_buffer, lowpass_in_buffer_size, lowpass_filter_def.size() - 1);
        enqueue_float_copy(*efm_equalization_in_buffer, efm_equalization_in_buffer_size, efm_equalization_filter_def.size() - 1);
        command_buffer->enqueueCopyBuffer(*dropout_buffer, *dropout_buffer, (dropout_buffer_size - dropout_delay) * sizeof(uint8_t), 0, dropout_delay * sizeof(uint8_t));

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
