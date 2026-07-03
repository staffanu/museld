//
// Created by staffanu on 12/10/23.
//

#ifndef MUSECPP_MUSERFDEMODULATOR_H
#define MUSECPP_MUSERFDEMODULATOR_H

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
#include "MuseConstants.h"
#include "efm/EfmDemodulator.h"

struct MuseDemodulatedBlock {
    static constexpr int c_sample_block_size = 512 * 1024;
    static constexpr int c_video_decimation_rate = 2;
    static constexpr int c_efm_decimation_rate = 4;

    static constexpr int c_video_block_size = c_sample_block_size / c_video_decimation_rate;
    static constexpr int c_efm_block_size = c_sample_block_size / c_efm_decimation_rate;

    // enough buffers for two frames
    static int recommendedNumberOfBlockBuffers(float sample_frequency) {
        return (int) (2 * MUSE_TOTAL_HEIGHT * MUSE_TOTAL_WIDTH
                      * sample_frequency / c_video_decimation_rate / 16.2e6 / c_video_block_size);
    }

    explicit MuseDemodulatedBlock(musevk::VulkanManager &vulkan_manager)
    : input_offset(0) {
        static_assert(c_sample_block_size % c_video_decimation_rate == 0);

        video_data = std::make_unique<musevk::VulkanBuffer>(
                vulkan_manager, musevk::Size(c_video_block_size), sizeof(float),
                vk::BufferUsageFlagBits::eStorageBuffer, musevk::HostAccess::eHostRead);
        dropouts = std::make_unique<musevk::VulkanBuffer>(
                vulkan_manager, musevk::Size(c_video_block_size), sizeof(uint8_t),
                vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst, musevk::HostAccess::eHostRead);
    }

    long input_offset; // the number of samples in the input before this block
    std::shared_ptr<musevk::VulkanBuffer> video_data;
    std::shared_ptr<musevk::VulkanBuffer> dropouts; // 1-to-1 with the video_data array. 0 or 1 for now, but could indicate how certain we are in the future
    std::vector<float> efm_input; // raw input samples staged for the EFM worker thread; empty when EFM is disabled
    std::vector<float> efm_data;
};

class MuseRfDemodulator : public RfDemodulator<MuseDemodulatedBlock> {
public:
    MuseRfDemodulator(Logger &log, std::string executable_dir, std::string filename, float sample_frequency,
                      musevk::VulkanManager &vulkan_manager, InputFormat input_format, bool benchmark_shaders,
                      bool efm_enabled);
    MuseRfDemodulator(const MuseRfDemodulator&) = delete;
    void operator=(const MuseRfDemodulator&) = delete;

    void setEfmEnabled(bool enabled) { m_efm_enabled = enabled; }

protected:
    void demodulate() override;

private:
    static constexpr float c_center_frequency = 12.5e6f;
    static constexpr float c_frequency_deviation = 1.9e6f;
    EfmDemodulator m_efm_demodulator;
    std::atomic<bool> m_efm_enabled;
};

#endif //MUSECPP_MUSERFDEMODULATOR_H
