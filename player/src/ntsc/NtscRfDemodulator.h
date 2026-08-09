// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_NTSCRFDEMODULATOR_H
#define MUSECPP_NTSCRFDEMODULATOR_H

#include <cstdint>
#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <format>
#include <cassert>
#include <complex>
#include <array>
#include <algorithm>
#include <thread>
#include <atomic>
#include <deque>
#include <condition_variable>
#include "logging/Logger.h"
#include "RfDemodulator.h"
#include "musevk/VulkanManager.h"
#include "efm/EfmDemodulator.h"
#include "analog/AnalogAudioDemodulator.h"

namespace NtscRfDemodulatorConstants {
    static constexpr int c_sample_block_size = 512 * 1024;
    static constexpr int c_video_decimation_rate = 2;
    static constexpr int c_audio_decimation_rate = 4;

    static constexpr int c_video_block_size = c_sample_block_size / c_video_decimation_rate;
    static constexpr int c_audio_block_size = c_sample_block_size / c_audio_decimation_rate;
}

struct NtscDemodulatedBlock {
    explicit NtscDemodulatedBlock(musevk::VulkanManager &vulkan_manager)
    : input_offset(0) {
        video_data = std::make_unique<musevk::VulkanBuffer>(
                vulkan_manager, musevk::Size(NtscRfDemodulatorConstants::c_video_block_size), sizeof(float),
                vk::BufferUsageFlagBits::eStorageBuffer, musevk::HostAccess::eHostRead);
        dropouts = std::make_unique<musevk::VulkanBuffer>(
                vulkan_manager, musevk::Size(NtscRfDemodulatorConstants::c_video_block_size), sizeof(uint8_t),
                vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst, musevk::HostAccess::eHostRead);;
        audio_data = std::make_unique<musevk::VulkanBuffer>(
                vulkan_manager, musevk::Size(NtscRfDemodulatorConstants::c_audio_block_size), sizeof(float),
                vk::BufferUsageFlagBits::eStorageBuffer, musevk::HostAccess::eHostRead);
    }

    int64_t input_offset; // the number of samples in the input before this block
    std::shared_ptr<musevk::VulkanBuffer> video_data;
    std::shared_ptr<musevk::VulkanBuffer> dropouts; // 1-to-1 with the video_data array. 0 or 1 for now, but could indicate how certain we are in the future
    std::vector<float> raw_input; // raw input samples staged for the audio worker thread; empty when EFM and analog are both disabled
    bool efm_wanted = false;      // which decoders the worker runs on raw_input
    bool analog_wanted = false;
    std::vector<float> efm_data;
    std::vector<TwoChannelSample> analog_data; // 48 kHz stereo from the analog FM carriers
    std::shared_ptr<musevk::VulkanBuffer> audio_data;
};

class NtscRfDemodulator : public RfDemodulator<NtscDemodulatedBlock> {
public:
    NtscRfDemodulator(Logger &log, std::string executable_dir, std::string filename, float sample_frequency,
                      musevk::VulkanManager &vulkan_manager, InputFormat input_format, bool benchmark_shaders,
                      bool efm_enabled);
    NtscRfDemodulator(const NtscRfDemodulator&) = delete;
    void operator=(const NtscRfDemodulator&) = delete;

    // Join the demodulator thread while demodulate() and m_efm_demodulator
    // still exist; the base destructor's cleanup() would be too late.
    ~NtscRfDemodulator() {
        cleanup();
    }

    void setEfmEnabled(bool enabled) { m_efm_enabled = enabled; }
    void setAnalogEnabled(bool enabled) { m_analog_enabled = enabled; }
    // CX expansion for the analog audio, driven by the VBI status decoded downstream
    void setAnalogCx(bool enabled) { m_analog_cx = enabled; }

    // enough buffers for two frames
    [[nodiscard]] int numberOfBlockBuffers() const {
        return std::max(2, (int)(2 * m_sample_frequency / 30 / NtscRfDemodulatorConstants::c_sample_block_size));
    }

protected:
    void demodulate() override;

private:
    static constexpr float c_center_frequency = 8.5e6f;
    static constexpr float c_frequency_deviation = 0.85e6f;
    EfmDemodulator m_efm_demodulator;
    AnalogAudioDemodulator m_analog_demodulator;
    std::atomic<bool> m_efm_enabled;
    std::atomic<bool> m_analog_enabled;
    std::atomic<bool> m_analog_cx;
};

#endif //MUSECPP_NTSCRFDEMODULATOR_H
