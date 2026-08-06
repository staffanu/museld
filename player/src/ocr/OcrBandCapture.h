// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_OCR_BAND_CAPTURE_H
#define MUSELD_OCR_BAND_CAPTURE_H

#include <cstdint>
#include <memory>
#include <vector>

#include "musevk/CommandPool.h"
#include "musevk/VulkanBuffer.h"
#include "musevk/VulkanImage.h"
#include "musevk/VulkanManager.h"

// Reads the bottom band of the decoded output image back to the CPU as packed
// sRGB RGB8 for the OCR worker.  Same readback mechanics as FrameExporter
// (whole-image copy + host-visible staging buffer), converting only the band.
class OcrBandCapture {
public:
    // band_fraction: the bottom fraction of the frame that may hold subtitles.
    OcrBandCapture(musevk::VulkanManager &manager, musevk::CommandPool &command_pool,
                   double band_fraction);

    // Synchronous: submits the copy and waits.  Returns packed RGB8 of the
    // band; width/height are set to the band dimensions.
    std::vector<uint8_t> capture(musevk::VulkanImage &image, int &width, int &height);

private:
    musevk::VulkanManager &m_manager;
    musevk::CommandPool &m_command_pool;
    double m_band_fraction;
    std::shared_ptr<musevk::VulkanBuffer> m_staging;
    std::shared_ptr<musevk::CommandBuffer> m_command_buffer;
};

#endif // MUSELD_OCR_BAND_CAPTURE_H
