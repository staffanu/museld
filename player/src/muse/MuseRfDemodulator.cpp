// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#include <chrono>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>
#include <iostream>
#include <filesystem>
#include "MuseRfDemodulator.h"
#include "filter/RaisedCosine.h"
#include "filter/WindowedSinc.h"
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
    // Match firdes' rectangular-window ntaps = (int)(21*Fs/(22*tw)) | 1
    std::vector<std::complex<float>> bandpass_filter_def =
            WindowedSinc::complex_band_pass<float>(WindowedSinc::rectangular_ntaps(m_sample_frequency, 2e6),
                                                   m_sample_frequency, 2.7e6, 22.3e6);
    std::reverse(bandpass_filter_def.begin(), bandpass_filter_def.end()); // shader correlates rather than convolves
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
            WindowedSinc::low_pass<float>(WindowedSinc::rectangular_ntaps(m_sample_frequency, 2e6),
                                          m_sample_frequency, 9.8e6);
    std::reverse(lowpass_filter_def.begin(), lowpass_filter_def.end()); // symmetric, but the shader correlates
    const int lowpass_filter_size = (int)lowpass_filter_def.size();

    shared_ptr<VulkanBuffer> lowpass_filter =
            VulkanUtil::createDeviceBuffer(m_vulkan_manager, command_pool, Size(lowpass_filter_size), lowpass_filter_def);

    // Root raised cosine filter for pulse shaping.
    // The RRC works on half the input sample rate (e.g. 31.25 MHz at default Fs=62.5 MHz).
    auto rrc_d = RaisedCosine::rrcFilter(11, m_sample_frequency / 2 / 16.2e6, 0.1,
                                         RaisedCosine::Normalization::eUnityDcGain);
    std::vector<float> rrc_filter_def(rrc_d.cbegin(), rrc_d.cend());
    std::reverse(rrc_filter_def.begin(), rrc_filter_def.end()); // symmetric, but the shader correlates
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

    // The CPU only writes to this mapping (the EFM path stages its input separately), but log the
    // memory properties since a non-host-cached (write-combined) mapping would make any future
    // CPU read of it very slow.
    m_log.info(ePerformance, std::format("input_buffer host mapping memory properties: {}{}",
                                         vk::to_string(input_buffer->hostMappedMemoryProperties()),
                                         (input_buffer->hostMappedMemoryProperties() & vk::MemoryPropertyFlagBits::eHostCached)
                                                 ? "" : " -- NOT host-cached: do not read from this mapping on the CPU"));

    shared_ptr<VulkanBuffer> analytic_buffer_re = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(analytic_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> analytic_buffer_im = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(analytic_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> lowpass_in_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(lowpass_in_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> rrc_in_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(rrc_in_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> dropout_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(c_dropout_buffer_size), sizeof(uint8_t), buffer_usage_flags, HostAccess::eHostNone);

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

    // The EFM demodulation is CPU work independent of the Vulkan pipeline, so it runs on its own
    // worker thread and overlaps the next block's read/record/submit.  The demodulation loop stages
    // the raw input samples in block->efm_input and hands the block over after the GPU has finished
    // with it; the worker fills in efm_data and forwards the block to m_filled_blocks.  A single
    // worker with a FIFO queue keeps the blocks in order, and the queue is bounded by the block
    // pool: when all blocks are queued for EFM the loop waits on m_cv_vacant as before.
    std::mutex efm_mutex;
    std::condition_variable efm_cv;
    std::deque<unique_ptr<MuseDemodulatedBlock>> efm_queue;
    bool efm_stop = false;
    std::thread efm_thread([&]() {
#ifdef __APPLE__
        pthread_setname_np("museld-efm");
#elif defined(linux)
        pthread_setname_np(pthread_self(), "museld-efm");
#endif
        while (true) {
            unique_ptr<MuseDemodulatedBlock> block;
            {
                std::unique_lock<std::mutex> lock(efm_mutex);
                efm_cv.wait(lock, [&] { return efm_stop || !efm_queue.empty(); });
                if (efm_queue.empty())
                    break; // stop requested and the queue is drained
                block = std::move(efm_queue.front());
                efm_queue.pop_front();
            }
            if (!block->efm_input.empty())
                m_efm_demodulator.demodulate(block->efm_input.data(), block->efm_data);
            else
                block->efm_data.clear();

            std::unique_lock<std::mutex> lock(m_demodulated_block_mutex);
            m_cv_filled.notify_one();
            m_filled_blocks.push_back(std::move(block));
        }
    });

    // Per-section CPU timing of the demodulation loop, reported every c_timing_report_blocks
    // blocks together with the real-time budget per block.  On a fifo the read time includes
    // waiting for the capture device, so a large read share is expected there.
    using timing_clock = std::chrono::steady_clock;
    constexpr int c_timing_report_blocks = 256;
    const double block_budget_ms = MuseDemodulatedBlock::c_sample_block_size / (double)m_sample_frequency * 1e3;
    double read_ms = 0, acquire_ms = 0, record_ms = 0, submit_ms = 0, efm_ms = 0, gpu_wait_ms = 0;
    int timed_blocks = 0;
    auto ms_between = [](timing_clock::time_point a, timing_clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };

    // When EFM is enabled the input samples are read into this heap staging buffer and copied to
    // the mapped Vulkan buffer, so that the EFM path never reads back from the mapping -- host
    // reads of a non-host-cached (write-combined) mapping are very slow on some hardware.
    std::vector<float> efm_staging;

    while (!m_stop_request) {
        auto t_loop_start = timing_clock::now();
        const bool efm_enabled = m_efm_enabled;
        float *input_samples = input_buffer->data<float>() + bandpass_filter_size - 1;
        if (efm_enabled) {
            efm_staging.resize(MuseDemodulatedBlock::c_sample_block_size);
            if (!readFloats(efm_staging.data(), MuseDemodulatedBlock::c_sample_block_size))
                break;
            memcpy(input_samples, efm_staging.data(), MuseDemodulatedBlock::c_sample_block_size * sizeof(float));
        } else if (!readFloats(input_samples, MuseDemodulatedBlock::c_sample_block_size)) {
            break;
        }
        auto t_after_read = timing_clock::now();

        // First get a free output block to write to
        unique_ptr<MuseDemodulatedBlock> block = nullptr;
        {
            std::unique_lock<std::mutex> lock(m_demodulated_block_mutex);
            if (m_input_is_fifo && m_vacant_blocks.empty() && !m_filled_blocks.empty()) {
                // discard a filled buffer -- this is better than waiting since it means the reader cannot cope anyway
                // (with all blocks in the EFM queue there is nothing to discard, so wait for the worker below)
                m_log.warn(eInput, "Discarding demodulated block due to overrun");
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
        auto t_after_acquire = timing_clock::now();
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

        // Barrier: ensure all compute shader writes are visible to the subsequent transfer operations
        command_buffer->enqueueBarrier(vk::AccessFlagBits::eShaderWrite,
                                       vk::AccessFlagBits::eTransferRead,
                                       vk::PipelineStageFlagBits::eComputeShader,
                                       vk::PipelineStageFlagBits::eTransfer);

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

        auto t_after_record = timing_clock::now();
        command_buffer->submit({}, {}, {});
        auto t_after_submit = timing_clock::now();

        // Hand the staged input samples to the block for the EFM worker.  The swap gives the block
        // this iteration's samples and reclaims the block's old buffer for the next read, so no
        // data is copied and nothing is read back from the mapped input buffer.
        if (efm_enabled)
            std::swap(block->efm_input, efm_staging);
        else
            block->efm_input.clear();
        auto t_after_efm = timing_clock::now();

        command_buffer->wait();
        auto t_after_gpu_wait = timing_clock::now();

        read_ms += ms_between(t_loop_start, t_after_read);
        acquire_ms += ms_between(t_after_read, t_after_acquire);
        record_ms += ms_between(t_after_acquire, t_after_record);
        submit_ms += ms_between(t_after_record, t_after_submit);
        efm_ms += ms_between(t_after_submit, t_after_efm);
        gpu_wait_ms += ms_between(t_after_efm, t_after_gpu_wait);
        if (++timed_blocks == c_timing_report_blocks) {
            m_log.info(ePerformance, std::format(
                    "demodulate avg/block (budget {:.2f} ms): read {:.2f} ms, acquire {:.2f} ms, "
                    "record {:.2f} ms, submit {:.2f} ms, efm copy {:.2f} ms, gpu wait {:.2f} ms, total {:.2f} ms",
                    block_budget_ms, read_ms / timed_blocks, acquire_ms / timed_blocks,
                    record_ms / timed_blocks, submit_ms / timed_blocks, efm_ms / timed_blocks,
                    gpu_wait_ms / timed_blocks,
                    (read_ms + acquire_ms + record_ms + submit_ms + efm_ms + gpu_wait_ms) / timed_blocks));
            read_ms = acquire_ms = record_ms = submit_ms = efm_ms = gpu_wait_ms = 0;
            timed_blocks = 0;
        }

        if (timestamp_query_pool != nullptr)
            timestamp_statistics.add_timestamps(timestamp_query_pool->getTimestamps());

        // Hand the block to the EFM worker, which forwards it to m_filled_blocks.  This must not
        // happen before command_buffer->wait() above, since the consumer reads video_data as soon
        // as the block appears in m_filled_blocks.
        std::unique_lock<std::mutex> lock(efm_mutex);
        efm_cv.notify_one();
        efm_queue.push_back(std::move(block));
    }

    // Let the EFM worker drain its queue before signalling end of stream
    {
        std::unique_lock<std::mutex> lock(efm_mutex);
        efm_stop = true;
        efm_cv.notify_one();
    }
    efm_thread.join();

    m_reader_thread_finished = true;
    m_cv_filled.notify_one();

    delete timestamp_query_pool;
    timestamp_statistics.print_stats(0);
}
