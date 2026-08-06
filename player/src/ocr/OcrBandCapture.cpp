// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "OcrBandCapture.h"

#include <algorithm>
#include <cmath>

#include "musevk/CommandBuffer.h"
#include "musevk/HalfFloatUtil.h"
#include "musevk/Size.h"

using namespace musevk;

namespace {

uint8_t encodeSrgb(float linear) {
    const float clamped = std::clamp(linear, 0.f, 1.f);
    const float s = clamped <= 0.0031308f ? 12.92f * clamped
                                          : 1.055f * std::pow(clamped, 1.f / 2.4f) - 0.055f;
    return (uint8_t)std::lround(s * 255.f);
}

} // namespace

OcrBandCapture::OcrBandCapture(VulkanManager &manager, CommandPool &command_pool,
                               double band_fraction)
        : m_manager(manager),
          m_command_pool(command_pool),
          m_band_fraction(band_fraction) {
}

std::vector<uint8_t> OcrBandCapture::capture(VulkanImage &image, int &width, int &height) {
    const uint32_t image_width = image.getWidth();
    const uint32_t image_height = image.getHeight();

    if (!m_staging) {
        // Four half-float components per rgba16f texel
        m_staging = std::make_shared<VulkanBuffer>(m_manager, Size(image_width * 4, image_height),
                                                   sizeof(uint16_t),
                                                   vk::BufferUsageFlagBits::eTransferDst, eHostRead);
        m_command_buffer = m_command_pool.createCommandBuffer();
    }

    m_command_buffer->begin();
    image.enqueueCopyToBuffer(*m_command_buffer, m_staging->buffer());
    m_command_buffer->enqueueBufferBarrier(*m_staging,
                                           vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferRead,
                                           vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer);
    m_command_buffer->enqueueBufferBarrier(*m_staging,
                                           vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eHostRead,
                                           vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost);
    m_staging->synchronizeForHostRead(*m_command_buffer);
    m_command_buffer->submit({}, {}, {});
    m_command_buffer->wait();

    const int band_top = (int)((double)image_height * (1.0 - m_band_fraction));
    width = (int)image_width;
    height = (int)image_height - band_top;

    const uint16_t *texels = m_staging->data<uint16_t>();
    std::vector<uint8_t> rgb((size_t)width * height * 3);
    for (int y = 0; y < height; y++) {
        const uint16_t *row = texels + (size_t)(band_top + y) * image_width * 4;
        for (int x = 0; x < width; x++)
            for (int c = 0; c < 3; c++)
                rgb[((size_t)y * width + x) * 3 + c] =
                        encodeSrgb(HalfFloatUtil::half_to_float(row[(size_t)x * 4 + c]));
    }
    return rgb;
}
