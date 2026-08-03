// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cstdint>
#include "SubtitleOverlay.h"

#include <algorithm>
#include <climits>

#include "Decoder.h"
#include "DiscInfo.h"
#include "PlayerState.h"
#include "ResultImages.h"
#include "musevk/CommandBuffer.h"
#include "musevk/Size.h"
#include "musevk/VulkanImage.h"
#include "musevk/VulkanUtil.h"

namespace {
constexpr int c_uints_per_quad = 8;

struct PushBlock {
    uint32_t n_rect;
    uint32_t n_glyph;
    uint32_t atlas_width;
    uint32_t atlas_height;
    float bg_r;
    float bg_g;
    float bg_b;
    float bg_a;
};
} // namespace

SubtitleOverlay::SubtitleOverlay(std::shared_ptr<const std::vector<SubtitleTrack>> tracks,
                                 std::shared_ptr<SubtitleFont> font,
                                 bool anchor_top,
                                 const std::string &executable_dir,
                                 musevk::VulkanManager &vulkan_manager,
                                 musevk::CommandPool & /*command_pool*/)
    : m_tracks(std::move(tracks)),
      m_font(std::move(font)),
      m_anchor_top(anchor_top),
      m_vulkan_manager(vulkan_manager) {

    m_quad_buffer = std::make_shared<musevk::VulkanBuffer>(
        m_vulkan_manager,
        musevk::Size(c_max_quads * c_uints_per_quad),
        sizeof(uint32_t),
        vk::BufferUsageFlagBits::eStorageBuffer,
        musevk::eHostWrite);

    m_shader = std::make_shared<musevk::ComputeShader>(
        m_vulkan_manager,
        "render_subtitle",
        std::vector<musevk::MemoryObjectType>{musevk::eBuffer, musevk::eBuffer, musevk::eImage},
        sizeof(PushBlock),
        musevk::VulkanUtil::loadSpirv(executable_dir, "render_subtitle.comp"),
        musevk::Size(0));
}

void SubtitleOverlay::writeQuad(uint32_t *dst, int dst_x, int dst_y, int w, int h, int atlas_x, int atlas_y) {
    dst[0] = static_cast<uint32_t>(std::max(0, dst_x));
    dst[1] = static_cast<uint32_t>(std::max(0, dst_y));
    dst[2] = static_cast<uint32_t>(std::max(0, w));
    dst[3] = static_cast<uint32_t>(std::max(0, h));
    dst[4] = static_cast<uint32_t>(std::max(0, atlas_x));
    dst[5] = static_cast<uint32_t>(std::max(0, atlas_y));
    dst[6] = 0;
    dst[7] = 0;
}

void SubtitleOverlay::render(musevk::CommandBuffer &command_buffer,
                             ResultImages &images,
                             PlayerState &state,
                             const Decoder & /*decoder*/,
                             int track_index) {
    if (track_index < 0 || track_index >= static_cast<int>(m_tracks->size())) return;
    const auto &entries = (*m_tracks)[track_index].entries;
    if (entries.empty() || !state.last_decoded.disc_info) return;
    auto t_opt = state.last_decoded.disc_info->playbackTimeSeconds();
    if (!t_opt) return;
    const double t = *t_opt;

    int active = -1;
    {
        auto it = std::upper_bound(entries.begin(), entries.end(), t,
            [](double v, const SubtitleEntry &e) { return v < e.start_seconds; });
        if (it != entries.begin()) {
            auto cand = std::prev(it);
            if (t < cand->end_seconds) active = static_cast<int>(cand - entries.begin());
        }
    }
    if (active < 0) return;

    if (active != m_last_entry_index || track_index != m_last_track_index) {
        m_last_track_index = track_index;
        m_last_entry_index = active;
        const auto &img = images.out_image;
        const int frame_w = static_cast<int>(img->getWidth());
        const int frame_h = static_cast<int>(img->getHeight());
        const int max_width = static_cast<int>(frame_w * 0.8);
        m_last_laid = m_font->layout(entries[active].lines, frame_w, frame_h, max_width, m_anchor_top);
    }
    if (m_last_laid.lines.empty()) return;

    // Pack [rects | glyphs] into the host-visible quad buffer.
    uint32_t *buf = m_quad_buffer->data<uint32_t>();
    uint32_t n_rect = 0;
    for (const auto &ll : m_last_laid.lines) {
        if (n_rect >= c_max_quads) break;
        writeQuad(buf + n_rect * c_uints_per_quad,
                  ll.bg_x, ll.bg_y, ll.bg_width, ll.bg_height, 0, 0);
        ++n_rect;
    }
    uint32_t n_glyph = 0;
    for (const auto &ll : m_last_laid.lines) {
        for (const auto &g : ll.glyphs) {
            if (n_rect + n_glyph >= c_max_quads) break;
            writeQuad(buf + (n_rect + n_glyph) * c_uints_per_quad,
                      g.dst_x, g.dst_y, g.width, g.height, g.atlas_x, g.atlas_y);
            ++n_glyph;
        }
        if (n_rect + n_glyph >= c_max_quads) break;
    }
    if (n_rect + n_glyph == 0) return;

    // Bounding box covering all quads — the dispatch grid.
    uint32_t min_x = UINT32_MAX, min_y = UINT32_MAX, max_x = 0, max_y = 0;
    for (uint32_t i = 0; i < n_rect + n_glyph; ++i) {
        uint32_t *q = buf + i * c_uints_per_quad;
        min_x = std::min(min_x, q[0]);
        min_y = std::min(min_y, q[1]);
        max_x = std::max(max_x, q[0] + q[2]);
        max_y = std::max(max_y, q[1] + q[3]);
    }
    if (max_x <= min_x || max_y <= min_y) return;

    m_quad_buffer->synchronizeHostWrites(command_buffer);

    const auto &img = images.out_image;
    img->enqueueTransitionLayout(command_buffer, vk::ImageLayout::eGeneral,
                                 vk::PipelineStageFlagBits::eTopOfPipe,
                                 vk::PipelineStageFlagBits::eComputeShader,
                                 vk::AccessFlags(),
                                 vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);

    auto current = m_shader->getBuffersForDescriptorSet(0);
    if (current.size() != 3 || current[2] != img) {
        m_shader->updateBufferDescriptorsInSet(0,
            {m_quad_buffer, m_font->atlasBuffer(), img});
    }

    m_shader->updateWorkgroup(musevk::Size(max_x, max_y));

    PushBlock pc{};
    pc.n_rect = n_rect;
    pc.n_glyph = n_glyph;
    pc.atlas_width = static_cast<uint32_t>(m_font->atlasWidth());
    pc.atlas_height = static_cast<uint32_t>(m_font->atlasHeight());
    pc.bg_r = 0.10f;
    pc.bg_g = 0.10f;
    pc.bg_b = 0.10f;
    pc.bg_a = 0.55f;

    std::vector<PushBlock> push{pc};
    command_buffer.enqueueComputeShader(m_shader, push);
}
