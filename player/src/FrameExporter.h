// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#ifndef MUSECPP_FRAMEEXPORTER_H
#define MUSECPP_FRAMEEXPORTER_H

#include <memory>
#include <optional>
#include <string>

class Logger;

namespace musevk {
    class VulkanManager;
    class CommandPool;
    class CommandBuffer;
    class VulkanImage;
    class VulkanBuffer;
}

// Reads back the decoded frame image from the GPU and writes it as a PNG file.
// The source is the same image the swapchain blit presents, so the file matches
// the displayed picture (minus OSD text and subtitles, which are drawn later).
class FrameExporter {
public:
    FrameExporter(Logger &log, musevk::VulkanManager &manager, musevk::CommandPool &command_pool);
    ~FrameExporter();

    // Writes the image to filename, or to a timestamped museld-*.png in the
    // current directory if none is given.  Returns the path written, or
    // std::nullopt on failure.
    std::optional<std::string> exportFrame(musevk::VulkanImage &image,
                                           std::optional<std::string> const &filename = std::nullopt);

private:
    Logger &m_log;
    musevk::VulkanManager &m_manager;
    musevk::CommandPool &m_command_pool;
    std::shared_ptr<musevk::VulkanBuffer> m_staging; // raw rgba16f texels; created on first export
    std::shared_ptr<musevk::CommandBuffer> m_command_buffer;
};

#endif //MUSECPP_FRAMEEXPORTER_H
