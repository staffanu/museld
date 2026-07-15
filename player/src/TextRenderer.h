// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_TEXTRENDERER_H
#define MUSECPP_TEXTRENDERER_H

#include "musevk/VulkanBuffer.h"
#include "musevk/VulkanManager.h"
#include "musevk/CommandPool.h"

class TextRenderer {
public:
    static constexpr int c_glyph_width = 12;
    static constexpr int c_glyph_height = 24;

    TextRenderer(std::string const &executable_dir, musevk::VulkanManager &vulkan_manager, musevk::CommandPool &command_pool);
    TextRenderer(const TextRenderer &) = delete;
    void operator=(const TextRenderer &) = delete;

    void drawText(std::shared_ptr<musevk::VulkanImage> const &image, int x, int y, std::string s, int scale, musevk::CommandBuffer &command_buffer);

private:
    static const int c_font_codepoint_begin = 32;
    static const int c_font_codepoint_end = 127; // exclusive
    static const std::vector<uint16_t> c_font_definition;

    musevk::VulkanManager &m_vulkan_manager;
    std::shared_ptr<musevk::VulkanBuffer> m_font_buffer;
    std::shared_ptr<musevk::ComputeShader> m_render_text_shader;
};

#endif //MUSECPP_TEXTRENDERER_H
