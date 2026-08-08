// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_NTSCINPUTBLOCK_H
#define MUSECPP_NTSCINPUTBLOCK_H

#include <cstdint>
#include <array>
#include "musevk/VulkanBuffer.h"
#include "InputBlockBase.h"

// A block contains a full frame of data, sampled at 14.3182 MHz = 4 * f_sc
class NtscInputBlock : public InputBlockBase {
public:
    NtscInputBlock(std::shared_ptr<musevk::VulkanBuffer> v, std::shared_ptr<musevk::VulkanBuffer> d)
            : InputBlockBase(),
              input_offset(0),
              input_samples_per_video_sample(0),
              video_data(std::move(v)),
              dropout_data(std::move(d)) {
    };

    static constexpr size_t c_requiredFileWriteBufferSize = 0;

    // 315/88 MHz * 4 = 14.3182 MHz = 910 * 525 * 30 / 1.001
    static constexpr int c_samples_per_video_line = 910;
    static constexpr double c_video_sampling_frequency = 315.0 / 88 * 1e6 * 4;
    static constexpr int c_total_video_lines = 525;


    int64_t input_offset;
    double input_samples_per_video_sample;
    std::shared_ptr<musevk::VulkanBuffer> video_data;
    std::shared_ptr<musevk::VulkanBuffer> dropout_data;

    void writeToFile(int fd, void *buffer) override {
        throw std::runtime_error("SD data cannot be written to file (file format remains to be defined)");
    }
};

template<>
class InputBlockFactory<NtscInputBlock> {
public:
    static std::unique_ptr<NtscInputBlock> makeBlock(musevk::VulkanManager &manager) {
        return std::make_unique<NtscInputBlock>(
                std::make_unique<musevk::VulkanBuffer>(manager, NtscInputBlock::c_samples_per_video_line * NtscInputBlock::c_total_video_lines,
                                                       sizeof(float), vk::BufferUsageFlagBits::eStorageBuffer, musevk::eHostWrite),
                std::make_unique<musevk::VulkanBuffer>(manager, NtscInputBlock::c_samples_per_video_line * NtscInputBlock::c_total_video_lines,
                                                       sizeof(uint8_t), vk::BufferUsageFlagBits::eStorageBuffer, musevk::eHostWrite)
        );
    }
};

#endif //MUSECPP_NTSCINPUTBLOCK_H
