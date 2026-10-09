// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_SDTVINPUTBLOCK_H
#define MUSECPP_SDTVINPUTBLOCK_H

#include <cstdint>
#include <array>
#include "musevk/VulkanBuffer.h"
#include "InputBlockBase.h"
#include "VideoStandard.h"

// A block contains a full frame of composite video, line locked on the
// standard's sampling grid (4 fsc = 14.3182 MHz for NTSC, 1135 samples per
// line = 17.734 MHz for PAL): total_lines rows of samples_per_line
class SdtvInputBlock : public InputBlockBase {
public:
    SdtvInputBlock(const VideoStandard &standard, std::shared_ptr<musevk::VulkanBuffer> v,
                   std::shared_ptr<musevk::VulkanBuffer> d)
            : InputBlockBase(),
              video_standard(standard),
              input_offset(0),
              input_samples_per_video_sample(0),
              timebase_restarted(false),
              video_data(std::move(v)),
              dropout_data(std::move(d)) {
    };

    static constexpr size_t c_requiredFileWriteBufferSize = 0;

    const VideoStandard &video_standard;

    int64_t input_offset;
    double input_samples_per_video_sample;
    // First frame after the reader lost the signal and re-acquired it (a
    // disc change on live input): per-disc statistics start over
    bool timebase_restarted;
    std::shared_ptr<musevk::VulkanBuffer> video_data;
    std::shared_ptr<musevk::VulkanBuffer> dropout_data;

    void writeToFile(int fd, void *buffer) override {
        throw std::runtime_error("SD data cannot be written to file (file format remains to be defined)");
    }
};

template<>
class InputBlockFactory<SdtvInputBlock> {
public:
    static std::unique_ptr<SdtvInputBlock> makeBlock(musevk::VulkanManager &manager, const VideoStandard &standard) {
        // video_data is written by the reader thread but also READ back by the
        // decoder's noise/level estimation, so it must not end up in uncached
        // (write-combined) memory: on discrete GPUs eHostWrite's first choice
        // is the PCIe BAR, where those reads run at a few MB/s and dominated a
        // whole frame's decode time.  eHostReadWrite picks a host-cached
        // mapping, or falls back to a cached staging buffer plus the device
        // copy that synchronizeHostWrites already maintains.  dropout_data is
        // only ever read by the GPU, so plain eHostWrite stays right for it.
        const size_t samples = (size_t)standard.samples_per_line * standard.total_lines;
        return std::make_unique<SdtvInputBlock>(
                standard,
                std::make_unique<musevk::VulkanBuffer>(manager, samples, sizeof(float),
                                                       vk::BufferUsageFlagBits::eStorageBuffer, musevk::eHostReadWrite),
                std::make_unique<musevk::VulkanBuffer>(manager, samples, sizeof(uint8_t),
                                                       vk::BufferUsageFlagBits::eStorageBuffer, musevk::eHostWrite)
        );
    }
};

#endif //MUSECPP_SDTVINPUTBLOCK_H
