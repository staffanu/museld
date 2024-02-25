//
// Created by staffanu on 2/25/24.
//

#ifndef MUSECPP_TEXTRENDERER_H
#define MUSECPP_TEXTRENDERER_H

#include "musevk/VulkanBuffer.h"
#include "musevk/VulkanManager.h"

class TextRenderer {
public:
    static constexpr int c_glyph_width = 6;
    static constexpr int c_glyph_height = 12;

    TextRenderer(std::string const &executable_dir, musevk::VulkanManager &vulkan_manager, std::shared_ptr<musevk::VulkanImage> const &image);
    TextRenderer(const TextRenderer &) = delete;
    void operator=(const TextRenderer &) = delete;

    void drawText(int x, int y, std::string s, int scale, musevk::CommandBuffer &command_buffer);

private:
    static const std::map<wchar_t, std::array<int, c_glyph_height>> c_monogram_font_definition;

    musevk::VulkanManager &m_vulkan_manager;
    std::shared_ptr<musevk::VulkanImage> m_image;

    std::shared_ptr<musevk::ComputeShader> m_render_text_shader;

    std::shared_ptr<musevk::VulkanBuffer> m_font_buffer;
    std::map<wchar_t, uint16_t> m_char_indices;
};

#endif //MUSECPP_TEXTRENDERER_H
