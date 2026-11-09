// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <string>
#include "MenuOverlay.h"
#include "Menu.h"
#include "PlayerState.h"
#include "ResultImages.h"
#include "TextRenderer.h"
#include "muse/MuseConstants.h"
#include "musevk/CommandBuffer.h"

void MenuOverlay::render(musevk::CommandBuffer &command_buffer, ResultImages &images, PlayerState &state,
                         const Menu &menu, TextRenderer &text_renderer) {
    if (!menu.isOpen())
        return;
    // The frame is re-decoded every pass while paused so the text does not pile up
    if (state.paused)
        state.redo_last_field = true;

    const int width = (int)images.out_image->getWidth();
    const int height = (int)images.out_image->getHeight();
    // The glyph scale as the OSD chooses it: tuned for MUSE's ~1032 lines, halved for SD
    constexpr int c_reference_height = 2 * MUSE_BUF_HEIGHT;
    const int scale = std::max(1, (2 * height + c_reference_height / 2) / c_reference_height);
    const int glyph_w = TextRenderer::c_glyph_width * scale;
    const int glyph_h = TextRenderer::c_glyph_height * scale;
    const int line_h = glyph_h + glyph_h / 3;
    const int pad = glyph_w;

    const auto lines = menu.lines();
    const std::string title = menu.title();
    // Column widths from the content, bounded by the frame: the key hint in
    // brackets at the left, the label, the value at the right
    size_t label_chars = title.size(), hint_chars = 0, value_chars = 0;
    for (const auto &line : lines) {
        label_chars = std::max(label_chars, line.label.size() + (line.is_page ? 2 : 0));
        if (!line.hint.empty())
            hint_chars = std::max(hint_chars, line.hint.size() + 2);
        value_chars = std::max(value_chars, line.value.size());
    }
    const int label_col = hint_chars ? (int)hint_chars + 1 : 0; // where the labels start, in glyphs
    const int gap = value_chars ? 3 : 0;
    const int panel_w = std::min(width - 2 * pad, pad * 2 + glyph_w * (label_col + (int)label_chars + gap + (int)value_chars));
    const int panel_h = pad * 2 + line_h * (int)(lines.size() + 1) + glyph_h / 2;
    const int x0 = pad * 2;
    const int y0 = std::max(pad, (height - panel_h) / 2);

    // The panel, the title bar and the selection bar; then the text on top
    text_renderer.fillRect(images.out_image, x0, y0, panel_w, panel_h, 0.0f, 0.0f, 0.0f, 0.7f, command_buffer);
    text_renderer.fillRect(images.out_image, x0, y0, panel_w, line_h + pad, 0.15f, 0.15f, 0.35f, 0.85f, command_buffer);
    int y = y0 + pad / 2;
    text_renderer.drawText(images.out_image, x0 + pad, y, title, scale, command_buffer);
    y += line_h + pad / 2 + glyph_h / 2;
    for (const auto &line : lines) {
        if (line.selected)
            text_renderer.fillRect(images.out_image, x0 + pad / 2, y - (line_h - glyph_h) / 2, panel_w - pad, line_h,
                                   0.9f, 0.9f, 0.9f, 0.25f, command_buffer);
        std::string label = line.label;
        if (line.is_page)
            label += " >";
        if (!line.hint.empty())
            text_renderer.drawText(images.out_image, x0 + pad, y, "[" + line.hint + "]", scale, command_buffer);
        text_renderer.drawText(images.out_image, x0 + pad + glyph_w * label_col, y, label, scale, command_buffer);
        if (!line.value.empty()) {
            // Right-aligned in the panel, cut from the left if it does not fit
            const int avail = (panel_w - 2 * pad) / glyph_w - label_col - (int)label.size() - gap;
            std::string value = line.value;
            if ((int)value.size() > avail && avail > 1)
                value = value.substr(value.size() - (size_t)avail);
            const int vx = x0 + panel_w - pad - glyph_w * (int)value.size();
            text_renderer.drawText(images.out_image, vx, y, value, scale, command_buffer);
        }
        y += line_h;
    }
}
