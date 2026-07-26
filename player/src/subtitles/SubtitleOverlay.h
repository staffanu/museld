// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_SUBTITLE_OVERLAY_H
#define MUSELD_SUBTITLE_OVERLAY_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "SrtParser.h"
#include "SubtitleFont.h"
#include "musevk/CommandPool.h"
#include "musevk/ComputeShader.h"
#include "musevk/VulkanBuffer.h"
#include "musevk/VulkanManager.h"

struct PlayerState;
struct ResultImages;
class Decoder;
namespace musevk { class CommandBuffer; }

class SubtitleOverlay {
public:
    SubtitleOverlay(std::vector<SubtitleEntry> entries,
                    std::unique_ptr<SubtitleFont> font,
                    const std::string &executable_dir,
                    musevk::VulkanManager &vulkan_manager,
                    musevk::CommandPool &command_pool);

    SubtitleOverlay(const SubtitleOverlay &) = delete;
    SubtitleOverlay &operator=(const SubtitleOverlay &) = delete;

    void render(musevk::CommandBuffer &command_buffer,
                ResultImages &images,
                PlayerState &state,
                const Decoder &decoder);

private:
    static constexpr int c_max_quads = 1024;

    void writeQuad(uint32_t *dst, int dst_x, int dst_y, int w, int h, int atlas_x, int atlas_y);

    std::vector<SubtitleEntry> m_entries;
    std::unique_ptr<SubtitleFont> m_font;
    int m_last_entry_index = -1;
    LaidSubtitle m_last_laid;

    musevk::VulkanManager &m_vulkan_manager;
    std::shared_ptr<musevk::VulkanBuffer> m_quad_buffer; // host-writable, max c_max_quads
    std::shared_ptr<musevk::ComputeShader> m_shader;
};

#endif // MUSELD_SUBTITLE_OVERLAY_H
