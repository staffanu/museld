// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_MUSEINPUTBLOCK_H
#define MUSECPP_MUSEINPUTBLOCK_H

#include <array>
#include <unistd.h>
#include "musevk/VulkanBuffer.h"
#include "musevk/VulkanManager.h"
#include "InputBlockBase.h"
#include "MuseConstants.h"

// A block contains a full frame of data, sampled at 16.2 MHz
class MuseInputBlock : public InputBlockBase {
public:
    MuseInputBlock(std::shared_ptr<musevk::VulkanBuffer> v, std::shared_ptr<musevk::VulkanBuffer> d)
            : InputBlockBase(),
              input_offset(0),
              input_samples_per_muse_sample(0),
              video_data(std::move(v)),
              dropout_data(std::move(d)) {
    };

    void writeToFile(int fd, void *buffer) {
        auto m_output_short_buffer = (short *)buffer;
        int size = video_data->size().numberOfElements();
        float *ptr = video_data->data<float>();
        for (int i = 0; i < size; i++)
            m_output_short_buffer[i] = ptr[i] * 4;

        if (write(fd, m_output_short_buffer, size * sizeof(int16_t)) != size * sizeof(int16_t))
            throw std::runtime_error("Output file write error");
    }

    static constexpr size_t c_requiredFileWriteBufferSize = sizeof(uint16_t) * MUSE_TOTAL_WIDTH * MUSE_TOTAL_HEIGHT;

    long input_offset;
    double input_samples_per_muse_sample;
    std::shared_ptr<musevk::VulkanBuffer> video_data;
    std::shared_ptr<musevk::VulkanBuffer> dropout_data;
};

template<>
class InputBlockFactory<MuseInputBlock> {
public:
    static std::unique_ptr<MuseInputBlock> makeBlock(musevk::VulkanManager &manager) {
        return std::make_unique<MuseInputBlock>(
                std::make_unique<musevk::VulkanBuffer>(manager, MUSE_TOTAL_HEIGHT * MUSE_TOTAL_WIDTH, sizeof(float), vk::BufferUsageFlagBits::eStorageBuffer, musevk::eHostWrite),
                std::make_unique<musevk::VulkanBuffer>(manager, MUSE_TOTAL_HEIGHT * MUSE_TOTAL_WIDTH, sizeof(uint8_t), vk::BufferUsageFlagBits::eStorageBuffer, musevk::eHostWrite)
        );
    }
};

#endif //MUSECPP_MUSEINPUTBLOCK_H
