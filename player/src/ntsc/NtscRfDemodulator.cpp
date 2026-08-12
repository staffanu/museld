// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cstdint>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <pthread.h>
#include <thread>
#include <utility>
#include <vector>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include "NtscRfDemodulator.h"
#include "filter/FirFilterStage.h"
#include "filter/WindowedSinc.h"
#include "musevk/VulkanUtil.h"
#include "musevk/TimestampQueryPool.h"
#include "musevk/TimestampStatistics.h"

using namespace std;
using namespace musevk;
using namespace NtscRfDemodulatorConstants;

NtscRfDemodulator::NtscRfDemodulator(Logger &log, std::string executable_dir, std::string filename,  float sample_frequency,
                                     musevk::VulkanManager &vulkan_manager, InputFormat input_format, bool benchmark_shaders,
                                     AudioTrack audio_track)
: RfDemodulator<NtscDemodulatedBlock>(log, std::move(executable_dir), std::move(filename), sample_frequency,
                                      vulkan_manager, input_format,
                                      NtscRfDemodulatorConstants::c_sample_block_size,
                                      benchmark_shaders),
  m_efm_demodulator(log, sample_frequency, NtscRfDemodulatorConstants::c_sample_block_size,
                    FirFilterStage::simdSupported(), true,
                    EfmDemodulator::defaultLog2Decimation(sample_frequency), 3, std::nullopt),
  m_analog_demodulator(log, sample_frequency, NtscRfDemodulatorConstants::c_sample_block_size,
                       48000.0, FirFilterStage::simdSupported()),
  m_ac3_demodulator(log, sample_frequency, NtscRfDemodulatorConstants::c_sample_block_size,
                    FirFilterStage::simdSupported()),
  m_ac3_decoder(log),
  m_efm_enabled(audio_track == AudioTrack::eEfm),
  m_analog_enabled(audio_track == AudioTrack::eDefault),
  m_ac3_enabled(audio_track == AudioTrack::eAc3),
  m_analog_cx(false) {
    // demodulateToSymbols requires block sizes aligned to its internal decimation
    assert(c_sample_block_size % m_ac3_demodulator.inputSampleAlignment() == 0);
}

void NtscRfDemodulator::demodulate() {
    assert(c_sample_block_size % c_video_decimation_rate == 0);

    CommandPool command_pool(m_vulkan_manager);
    std::unique_ptr<musevk::TimestampQueryPool> timestamp_query_pool = m_benchmark_shaders
            ? std::make_unique<musevk::TimestampQueryPool>(m_vulkan_manager.getPhysicalDevice(), m_vulkan_manager.getDevice(), 40)
            : nullptr;
    musevk::TimestampStatistics timestamp_statistics;
    auto command_buffer = command_pool.createCommandBuffer(timestamp_query_pool.get());

    vk::BufferUsageFlags buffer_usage_flags =
            vk::BufferUsageFlagBits::eStorageBuffer
            | vk::BufferUsageFlagBits::eTransferDst
            | vk::BufferUsageFlagBits::eTransferSrc;


    // Create all the filters

    // FIR band-pass filter that also creates an analytic signal.  The passband is 3.5 to 13.5 MHz.
    // Reverse because the FIR shader correlates rather than convolves.
    std::vector<std::complex<float>> bandpass_filter_def =
            WindowedSinc::complex_band_pass<float>(WindowedSinc::rectangular_ntaps(m_sample_frequency, 1.5e6),
                                                   m_sample_frequency, 3.5e6, 13.5e6);
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
            WindowedSinc::low_pass<float>(WindowedSinc::rectangular_ntaps(m_sample_frequency, 2e6), m_sample_frequency, 5e6);
    std::reverse(lowpass_filter_def.begin(), lowpass_filter_def.end()); // symmetric, but the shader correlates

    shared_ptr<VulkanBuffer> lowpass_filter =
            VulkanUtil::createDeviceBuffer(m_vulkan_manager, command_pool, Size(lowpass_filter_def.size()), lowpass_filter_def);

    // FIR cleanup lowpass at the decimated rate.  The video de-emphasis is NOT
    // applied here: it lives in the frame domain (ntsc_copy_to_frame.comp),
    // where the line-locked 4 fsc grid makes its coefficients independent of
    // the capture sample rate.
    const float decimated_frequency = m_sample_frequency / c_video_decimation_rate;
    std::vector<float> decimated_lowpass_filter_def =
            WindowedSinc::low_pass<float>(WindowedSinc::rectangular_ntaps(decimated_frequency, 5e6), decimated_frequency, 5e6);
    std::reverse(decimated_lowpass_filter_def.begin(), decimated_lowpass_filter_def.end()); // symmetric, but the shader correlates

    shared_ptr<VulkanBuffer> decimated_lowpass_filter =
            VulkanUtil::createDeviceBuffer(m_vulkan_manager, command_pool, Size(decimated_lowpass_filter_def.size()), decimated_lowpass_filter_def);

    // Each shader is specialized to the longest filter and largest decimation it will be
    // dispatched with, which is what its shared memory is then sized from.  input_fir_filter
    // runs the band-pass twice without decimating; fir_filter runs the low-pass decimating by
    // two and then the de-emphasis filter at the decimated rate.
    const uint32_t input_fir_max_filter_size = bandpass_filter_def.size();
    const uint32_t input_fir_max_decimation = 1;
    const uint32_t fir_max_filter_size = std::max(lowpass_filter_def.size(), decimated_lowpass_filter_def.size());
    const uint32_t fir_max_decimation = c_video_decimation_rate;

    // Create buffers for data
    const int input_buffer_size = c_sample_block_size + (int)bandpass_filter_def.size() - 1;
    const int analytic_buffer_size = c_sample_block_size + 1;
    const int lowpass_in_buffer_size = c_sample_block_size + (int)lowpass_filter_def.size() - 1;
    const int decimated_lowpass_in_buffer_size = c_sample_block_size + (int)decimated_lowpass_filter_def.size() - 1;

    // We need to delay the output of the detected dropouts as much as the rest of the filter chain delays the video signal
    const int dropout_delay = (int)lowpass_filter_def.size() / c_video_decimation_rate / 2 - 1;
    const int dropout_buffer_size = c_video_block_size + dropout_delay;

    shared_ptr<VulkanBuffer> input_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(input_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostWrite);

    // The CPU only writes to this mapping (the audio path stages its input separately), but log the
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

    shared_ptr<VulkanBuffer> equalization_in_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(decimated_lowpass_in_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> dropout_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(dropout_buffer_size), sizeof(uint8_t), buffer_usage_flags, HostAccess::eHostNone);


    // Create shaders
    shared_ptr<ComputeShader> input_fir_filter_shader = unique_ptr<ComputeShader>(
            new ComputeShader(m_vulkan_manager, "input_fir_filter",
                              {eBuffer, eBuffer, eBuffer}, 4 * sizeof(uint32_t),
                              VulkanUtil::loadSpirv(m_executable_dir, "input_fir_filter.comp"), Size(0), 2,
                              {input_fir_max_filter_size, input_fir_max_decimation}));

    input_fir_filter_shader->updateBufferDescriptorsInSet(0, {bandpass_filter_re, input_buffer, analytic_buffer_re});
    input_fir_filter_shader->updateBufferDescriptorsInSet(1, {bandpass_filter_im, input_buffer, analytic_buffer_im});

    shared_ptr<ComputeShader> fir_filter_shader = unique_ptr<ComputeShader>(
            new ComputeShader(m_vulkan_manager, "fir_filter",
                              {eBuffer, eBuffer, eBuffer}, 4 * sizeof(uint32_t),
                              VulkanUtil::loadSpirv(m_executable_dir, "fir_filter.comp"), Size(0), 2,
                              {fir_max_filter_size, fir_max_decimation}));

    fir_filter_shader->updateBufferDescriptorsInSet(0, {lowpass_filter, lowpass_in_buffer, equalization_in_buffer});

    checkFirShaderFits("input_fir_filter.comp", *input_fir_filter_shader,
                       input_fir_max_filter_size, input_fir_max_decimation);
    checkFirShaderFits("fir_filter.comp", *fir_filter_shader, fir_max_filter_size, fir_max_decimation);

    shared_ptr<ComputeShader> fm_quadrature_shader = unique_ptr<ComputeShader>(
            new ComputeShader(m_vulkan_manager, "fm_quadrature",
                              {analytic_buffer_re, analytic_buffer_im, lowpass_in_buffer}, 7 * sizeof(float),
                              VulkanUtil::loadSpirv(m_executable_dir, "fm_quadrature.comp"), Size(c_sample_block_size)));

    shared_ptr<ComputeShader> detect_dropouts_shader = unique_ptr<ComputeShader>(
            new ComputeShader(m_vulkan_manager,
                              "detect_dropouts_envelope",
                              {analytic_buffer_re, analytic_buffer_im, dropout_buffer, lowpass_in_buffer}, 10 * sizeof(uint32_t),
                              VulkanUtil::loadSpirv(m_executable_dir, "detect_dropouts_envelope.comp"), Size(NtscRfDemodulatorConstants::c_video_block_size)));

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

    // The EFM and analog audio demodulation is CPU work independent of the Vulkan pipeline, so it
    // runs on its own worker thread and overlaps the next block's read/record/submit.  The
    // demodulation loop stages the raw input samples in block->raw_input and hands the block over
    // after the GPU has finished with it; the worker fills in efm_data/analog_data and forwards
    // the block to m_filled_blocks.  A single
    // worker with a FIFO queue keeps the blocks in order, and the queue is bounded by the block
    // pool: when all blocks are queued for EFM the loop waits on m_cv_vacant as before.
    // The worker's shared state and the thread live in one struct so a single
    // destructor stops and joins on every exit path: an exception below would
    // otherwise unwind past a joinable std::thread (std::terminate) while
    // destroying the queue the worker is still using.
    struct EfmWorker {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<unique_ptr<NtscDemodulatedBlock>> queue;
        bool stop = false;
        std::thread thread; // last member: joined before the state above goes away

        ~EfmWorker() {
            {
                std::unique_lock<std::mutex> lock(mutex);
                stop = true;
                cv.notify_one();
            }
            if (thread.joinable())
                thread.join();
        }
    } efm;
    efm.thread = std::thread([&]() {
#ifdef __APPLE__
        pthread_setname_np("museld-efm");
#elif defined(linux) || defined(__FreeBSD__)
        pthread_setname_np(pthread_self(), "museld-efm");
#endif
        // Per-block CPU timing of the audio demodulation, reported every
        // c_timing_report_blocks blocks together with the real-time budget per
        // block.  Unlike MUSE, the NTSC path always runs one of the two
        // demodulators, so this stage can be the pipeline bottleneck.
        using timing_clock = std::chrono::steady_clock;
        constexpr int c_timing_report_blocks = 256;
        const double block_budget_ms = c_sample_block_size / (double)m_sample_frequency * 1e3;
        double efm_ms = 0, analog_ms = 0, ac3_ms = 0;
        int timed_blocks = 0;
        auto ms_between = [](timing_clock::time_point a, timing_clock::time_point b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        while (true) {
            unique_ptr<NtscDemodulatedBlock> block;
            {
                std::unique_lock<std::mutex> lock(efm.mutex);
                efm.cv.wait(lock, [&] { return efm.stop || !efm.queue.empty(); });
                if (efm.queue.empty())
                    break; // stop requested and the queue is drained
                block = std::move(efm.queue.front());
                efm.queue.pop_front();
            }
            auto t_block_start = timing_clock::now();
            if (block->efm_wanted)
                m_efm_demodulator.demodulate(block->raw_input.data(), block->efm_data);
            else
                block->efm_data.clear();
            auto t_after_efm = timing_clock::now();
            block->analog_data.clear();
            if (block->analog_wanted)
                m_analog_demodulator.demodulate(block->raw_input.data(), m_analog_cx, block->analog_data);
            auto t_after_analog = timing_clock::now();
            if (block->ac3_wanted)
                block->ac3_frames = m_ac3_decoder.decodeSymbols(
                        m_ac3_demodulator.demodulateToSymbols(block->raw_input.data(), c_sample_block_size));
            else
                block->ac3_frames.clear();
            auto t_after_ac3 = timing_clock::now();

            efm_ms += ms_between(t_block_start, t_after_efm);
            analog_ms += ms_between(t_after_efm, t_after_analog);
            ac3_ms += ms_between(t_after_analog, t_after_ac3);
            if (++timed_blocks == c_timing_report_blocks) {
                m_log.info(ePerformance, std::format(
                        "audio demod avg/block (budget {:.2f} ms): efm {:.2f} ms, analog {:.2f} ms, ac3 {:.2f} ms",
                        block_budget_ms, efm_ms / timed_blocks, analog_ms / timed_blocks, ac3_ms / timed_blocks));
                efm_ms = analog_ms = ac3_ms = 0;
                timed_blocks = 0;
            }

            std::unique_lock<std::mutex> lock(m_demodulated_block_mutex);
            m_cv_filled.notify_one();
            m_filled_blocks.push_back(std::move(block));
        }
    });

    // Per-section CPU timing of the demodulation loop, reported every c_timing_report_blocks
    // blocks together with the real-time budget per block.  On a fifo the read time includes
    // waiting for the capture device, so a large read share is expected there.  A large acquire
    // share means the downstream stages (audio demod worker or the reader's DPLL) cannot keep up.
    using timing_clock = std::chrono::steady_clock;
    constexpr int c_timing_report_blocks = 256;
    const double block_budget_ms = c_sample_block_size / (double)m_sample_frequency * 1e3;
    double read_ms = 0, acquire_ms = 0, record_ms = 0, submit_ms = 0, audio_ms = 0, gpu_wait_ms = 0;
    int timed_blocks = 0;
    auto ms_between = [](timing_clock::time_point a, timing_clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };

    // The samples are always read into this heap buffer and copied into the mapping in one go,
    // never written to the mapping directly.  The mapping is not host-cached, and writing it is
    // only fast when whole cache lines arrive at once: a reader that interleaves its stores with
    // other work -- the FLAC readers decode a frame between every 4096 samples -- leaves the CPU's
    // few write-combining buffers flushing partial lines, which more than doubles the time a
    // compressed input spends here.  It also keeps the EFM path from reading back from the mapping.
    std::vector<float> input_staging;

    while (!m_stop_request) {
        auto t_loop_start = timing_clock::now();
        const bool efm_enabled = m_efm_enabled;
        const bool analog_enabled = m_analog_enabled;
        const bool ac3_enabled = m_ac3_enabled;
        float *input_samples = input_buffer->data<float>() + bandpass_filter_def.size() - 1;
        input_staging.resize(c_sample_block_size);
        if (!readFloats(input_staging.data(), c_sample_block_size))
            break;
        memcpy(input_samples, input_staging.data(), c_sample_block_size * sizeof(float));
        auto t_after_read = timing_clock::now();

        // First get a free output block to write to
        unique_ptr<NtscDemodulatedBlock> block = nullptr;
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
                m_log.info(eInput, "NtscRfDemodulator: stop requested");
                break;
            }
            block = std::move(m_vacant_blocks.front());
            m_vacant_blocks.pop_front();
        }
        auto t_after_acquire = timing_clock::now();
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
                                                     m_sample_frequency, c_frequency_deviation, c_center_frequency, /* scale */ 0.5f, /* add */ 0.5f});

        // Lowpass filter the demodulated signal, and down-sample (decimate by factor 2)
        fir_filter_shader->updateWorkgroup(Size(c_video_block_size));
        command_buffer->enqueueComputeShader<uint32_t>(
                fir_filter_shader,
                {(uint32_t)lowpass_filter_def.size(), c_video_block_size, /* out offset */ 0, c_video_decimation_rate}, 0);

        // Run the down-sampled signal through the de-emphasis filter and store in the output block
        fir_filter_shader->updateBufferDescriptorsInSet(1, {decimated_lowpass_filter, equalization_in_buffer, block->video_data});
        command_buffer->enqueueComputeShader<uint32_t>(
                fir_filter_shader,
                {(uint32_t)decimated_lowpass_filter_def.size(), c_video_block_size, /* out offset */ 0, /* decimation */ 1}, 1);

        // Detect dropouts from the RF envelope, against two references: the
        // same position on the neighbouring lines (brightness-matched, since
        // the disc MTF lowers the envelope at the bright end of the deviation
        // range) with a 0.7 amplitude ratio, and the surrounding ~200 us
        // average with a strict 0.5 for dropouts that span several lines.
        // The analytic signal is written with offset 1 into its buffers, and
        // the flags are delayed like the video to compensate the decimating
        // lowpass.
        const uint32_t line_period = (uint32_t)lround(m_sample_frequency / (30000.0 / 1001.0 * 525.0));
        const float slew_threshold = 1.0f * 40e6f / m_sample_frequency; // legal slew scales with the sample interval
        // 32 taps spanning ~205 us, so a maximum-length dropout cannot drag the
        // carrier reference down with it
        const uint32_t long_stride = (uint32_t)lround(6.4e-6 * m_sample_frequency);
        command_buffer->enqueueComputeShader<uint32_t>(
                detect_dropouts_shader, {NtscRfDemodulatorConstants::c_video_block_size, 1u, (uint32_t)dropout_delay, c_video_decimation_rate,
                                         line_period,
                                         std::bit_cast<uint32_t>(0.49f), std::bit_cast<uint32_t>(0.25f),
                                         (uint32_t)lowpass_filter_def.size() - 1, std::bit_cast<uint32_t>(slew_threshold),
                                         long_stride});

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
        // The tail recycle below writes the start of dropout_buffer, which the
        // copy into block->dropouts above reads; transfers on the same queue
        // have no implicit ordering, so the write needs an execution dependency
        // on the read.  (The float recycles are ordered against the shader
        // reads by the compute-to-transfer barrier already.)
        command_buffer->enqueueBarrier(vk::AccessFlagBits::eTransferRead,
                                       vk::AccessFlagBits::eTransferWrite,
                                       vk::PipelineStageFlagBits::eTransfer,
                                       vk::PipelineStageFlagBits::eTransfer);
        command_buffer->enqueueCopyBuffer(*dropout_buffer, *dropout_buffer, (dropout_buffer_size - dropout_delay) * sizeof(uint8_t), 0, dropout_delay * sizeof(uint8_t));

        auto t_after_record = timing_clock::now();
        command_buffer->submit({}, {}, {});
        auto t_after_submit = timing_clock::now();

        // Hand the staged input samples to the block for the audio worker.  The swap gives the
        // block this iteration's samples and reclaims the block's old buffer for the next read, so
        // no data is copied and nothing is read back from the mapped input buffer.
        block->efm_wanted = efm_enabled;
        block->analog_wanted = analog_enabled;
        block->ac3_wanted = ac3_enabled;
        if (efm_enabled || analog_enabled || ac3_enabled)
            std::swap(block->raw_input, input_staging);
        else
            block->raw_input.clear();
        auto t_after_audio = timing_clock::now();

        command_buffer->wait();
        auto t_after_gpu_wait = timing_clock::now();

        read_ms += ms_between(t_loop_start, t_after_read);
        acquire_ms += ms_between(t_after_read, t_after_acquire);
        record_ms += ms_between(t_after_acquire, t_after_record);
        submit_ms += ms_between(t_after_record, t_after_submit);
        audio_ms += ms_between(t_after_submit, t_after_audio);
        gpu_wait_ms += ms_between(t_after_audio, t_after_gpu_wait);
        if (++timed_blocks == c_timing_report_blocks) {
            m_log.info(ePerformance, std::format(
                    "demodulate avg/block (budget {:.2f} ms): read {:.2f} ms, acquire {:.2f} ms, "
                    "record {:.2f} ms, submit {:.2f} ms, audio copy {:.2f} ms, gpu wait {:.2f} ms, total {:.2f} ms",
                    block_budget_ms, read_ms / timed_blocks, acquire_ms / timed_blocks,
                    record_ms / timed_blocks, submit_ms / timed_blocks, audio_ms / timed_blocks,
                    gpu_wait_ms / timed_blocks,
                    (read_ms + acquire_ms + record_ms + submit_ms + audio_ms + gpu_wait_ms) / timed_blocks));
            read_ms = acquire_ms = record_ms = submit_ms = audio_ms = gpu_wait_ms = 0;
            timed_blocks = 0;
        }

        if (timestamp_query_pool != nullptr)
            timestamp_statistics.add_timestamps(timestamp_query_pool->getTimestamps());

        // Hand the block to the EFM worker, which forwards it to m_filled_blocks.  This must not
        // happen before command_buffer->wait() above, since the consumer reads video_data as soon
        // as the block appears in m_filled_blocks.
        std::unique_lock<std::mutex> lock(efm.mutex);
        efm.cv.notify_one();
        efm.queue.push_back(std::move(block));
    }

    // Let the EFM worker drain its queue before signalling end of stream
    {
        std::unique_lock<std::mutex> lock(efm.mutex);
        efm.stop = true;
        efm.cv.notify_one();
    }
    efm.thread.join();

    m_reader_thread_finished = true;
    m_cv_filled.notify_one();

    timestamp_statistics.print_stats(0);
}
