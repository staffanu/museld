// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#include "FrameExporter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <vector>

#include "logging/Logger.h"
#include "musevk/CommandPool.h"
#include "musevk/HalfFloatUtil.h"
#include "musevk/VulkanBuffer.h"
#include "musevk/VulkanImage.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

using namespace musevk;

FrameExporter::FrameExporter(Logger &log, VulkanManager &manager, CommandPool &command_pool)
        : m_log(log),
          m_manager(manager),
          m_command_pool(command_pool) {
}

FrameExporter::~FrameExporter() = default;

static uint8_t encodeSrgb(float linear) {
    const float clamped = std::clamp(linear, 0.f, 1.f);
    const float s = clamped <= 0.0031308f ? 12.92f * clamped
                                          : 1.055f * std::pow(clamped, 1.f / 2.4f) - 0.055f;
    return (uint8_t)std::lround(s * 255.f);
}

static std::string makeTimestampedFilename() {
    time_t now = time(nullptr);
    tm tm_now{};
#ifdef _WIN32
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif
    char base[64];
    strftime(base, sizeof(base), "museld-%Y%m%d-%H%M%S", &tm_now);
    std::string name = std::format("{}.png", base);
    for (int i = 2; std::filesystem::exists(name); i++)
        name = std::format("{}-{}.png", base, i);
    return name;
}

std::optional<std::string> FrameExporter::exportFrame(VulkanImage &image,
                                                      std::optional<std::string> const &filename) {
    const uint32_t width = image.getWidth();
    const uint32_t height = image.getHeight();

    if (!m_staging) {
        // Four half-float components per rgba16f texel
        m_staging = std::make_shared<VulkanBuffer>(m_manager, Size(width * 4, height), sizeof(uint16_t),
                                                   vk::BufferUsageFlagBits::eTransferDst, eHostRead);
        m_command_buffer = m_command_pool.createCommandBuffer();
    }

    m_command_buffer->begin();
    image.enqueueCopyToBuffer(*m_command_buffer, m_staging->buffer());
    // synchronizeForHostRead orders against compute-shader writes; the producer
    // here is the transfer copy above, so cover that with explicit barriers
    m_command_buffer->enqueueBufferBarrier(*m_staging,
                                           vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferRead,
                                           vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer);
    m_command_buffer->enqueueBufferBarrier(*m_staging,
                                           vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eHostRead,
                                           vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost);
    m_staging->synchronizeForHostRead(*m_command_buffer);
    m_command_buffer->submit({}, {}, {});
    m_command_buffer->wait();

    // The shaders store linear-light values with sRGB primaries; the swapchain
    // blit applies the sRGB transfer function on display, and so do we here
    const uint16_t *texels = m_staging->data<uint16_t>();
    std::vector<uint8_t> rgb((size_t)width * height * 3);
    for (size_t p = 0; p < (size_t)width * height; p++)
        for (int c = 0; c < 3; c++)
            rgb[p * 3 + c] = encodeSrgb(HalfFloatUtil::half_to_float(texels[p * 4 + c]));

    const std::string path = filename ? *filename : makeTimestampedFilename();
    if (!stbi_write_png(path.c_str(), (int)width, (int)height, 3, rgb.data(), (int)(width * 3))) {
        m_log.error(eApplication, std::format("Failed to write {}", path));
        return std::nullopt;
    }
    m_log.info(eApplication, std::format("Exported frame to {}", path));
    return path;
}
