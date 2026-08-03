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

// One selectable subtitle stream: the label shown in the OSD when cycling with
// the [ and ] keys, and the parsed cues.
struct SubtitleTrack {
    std::string label;
    std::vector<SubtitleEntry> entries;
};

// Renders one of the loaded tracks into the output image, either bottom-anchored
// (the primary track) or top-anchored (the secondary track). The track list and
// font are shared between the two overlay instances; which track this instance
// shows is passed to render() each frame, so the hotkeys can switch freely.
class SubtitleOverlay {
public:
    SubtitleOverlay(std::shared_ptr<const std::vector<SubtitleTrack>> tracks,
                    std::shared_ptr<SubtitleFont> font,
                    bool anchor_top,
                    const std::string &executable_dir,
                    musevk::VulkanManager &vulkan_manager,
                    musevk::CommandPool &command_pool);

    SubtitleOverlay(const SubtitleOverlay &) = delete;
    SubtitleOverlay &operator=(const SubtitleOverlay &) = delete;

    // track_index selects into the track list; -1 (or out of range) shows nothing.
    void render(musevk::CommandBuffer &command_buffer,
                ResultImages &images,
                PlayerState &state,
                const Decoder &decoder,
                int track_index);

private:
    static constexpr int c_max_quads = 1024;

    void writeQuad(uint32_t *dst, int dst_x, int dst_y, int w, int h, int atlas_x, int atlas_y);

    std::shared_ptr<const std::vector<SubtitleTrack>> m_tracks;
    std::shared_ptr<SubtitleFont> m_font;
    bool m_anchor_top;
    int m_last_track_index = -1;
    int m_last_entry_index = -1;
    LaidSubtitle m_last_laid;

    musevk::VulkanManager &m_vulkan_manager;
    std::shared_ptr<musevk::VulkanBuffer> m_quad_buffer; // host-writable, max c_max_quads
    std::shared_ptr<musevk::ComputeShader> m_shader;
};

#endif // MUSELD_SUBTITLE_OVERLAY_H
