// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "OsdOverlay.h"

#include <algorithm>
#include <format>
#include <vector>
#include <GLFW/glfw3.h>

#include "Decoder.h"
#include "PlayerState.h"
#include "ResultImages.h"
#include "TextRenderer.h"
#include "DiscInfo.h"
#include "muse/MuseConstants.h"
#include "musevk/CommandBuffer.h"

std::string OsdOverlay::render(musevk::CommandBuffer &command_buffer,
                               ResultImages &images,
                               PlayerState &state,
                               const Decoder &decoder,
                               GLFWwindow *window,
                               TextRenderer &text_renderer) {
    std::string cursor_string;
    const int zoom = state.zoom_factor;
    const auto src = decoder.getSourceDimensions();
    const int vis_left = (int)((state.zoom_center.first - 0.5 / zoom) * src.width);
    const int vis_top = (int)((state.zoom_center.second - 0.5 / zoom) * src.height);
    const int vis_height = (int)(src.height / zoom);
    auto tl_x = [&](int x) { return vis_left + x / zoom; };
    auto tl_y = [&](int y) { return vis_top + y / zoom; };
    auto comp_scale = [&](int s) { return std::max(1, s / zoom); };

    // The glyph scale is tuned for MUSE's ~1032-line field; scale it down for
    // lower-resolution inputs (NTSC is ~480 lines) so the overlay occupies a
    // similar fraction of the picture instead of running off the screen.
    constexpr int c_reference_height = 2 * MUSE_BUF_HEIGHT; // MUSE field height
    const int osd_base = std::max(1, (4 * src.height + c_reference_height / 2) / c_reference_height);

    if (!state.osd_text.empty()) {
        if (state.paused && state.osd_text_remaining_frames)
            state.redo_last_field = true;
        state.osd_text_remaining_frames = 100;
        state.displayed_osd_text = state.osd_text;
        state.osd_text.clear();
    }
    if (state.osd_text_remaining_frames) {
        text_renderer.drawText(images.out_image, tl_x(90), tl_y(50), state.displayed_osd_text, comp_scale(osd_base), command_buffer);
        if (!--state.osd_text_remaining_frames) {
            state.redo_last_field = true;
            state.displayed_osd_text.clear();
        }
    }

    if (state.enable_cursor) {
        // Draw the pointer coordinates in the top left corner (both in single field and
        // interpolated coordinates). If paused and showing only a single field, also show
        // the input stream offset for the start of the frame and for pixels under the
        // pointer (Y, Cr, Cb).
        int xsize, ysize;
        glfwGetWindowSize(window, &xsize, &ysize);
        double xpos, ypos;
        glfwGetCursorPos(window, &xpos, &ypos);
        if (xpos >= 0 && ypos >= 0 && xpos < xsize && ypos < ysize) {
            double rel_x = (xpos / xsize - 0.5) / state.zoom_factor + state.zoom_center.first;
            double rel_y = (ypos / ysize - 0.5) / state.zoom_factor + state.zoom_center.second;
            int field_x = (int)(rel_x * src.field_width);
            int field_y = (int)(rel_y * src.field_height);
            cursor_string = std::format("({}, {}) ({}, {})",
                                            (int)(rel_x * src.width),
                                            (int)(rel_y * src.height),
                                            field_x, field_y);
            text_renderer.drawText(images.out_image, tl_x(10), vis_top + 8, cursor_string, 1, command_buffer);
            if (state.field_interpolation_mode == Decoder::FieldInterpolationMode::eForceIntraField && state.paused && zoom < 4) {
                if (auto offsets = decoder.computePixelFileOffsets(
                            field_x, field_y, state.last_decoded.field_parity,
                            state.last_decoded.last_frame_buffer_input_offset,
                            state.last_decoded.input_samples_per_muse_sample)) {
                    // Component formats report a luma (Y) sample; composite
                    // formats (NTSC) carry a single CVBS sample instead,
                    // distinguished here by the absent chroma offsets.
                    const char *sample_label = offsets->cr && offsets->cb ? "Y" : "CVBS";
                    std::string offset_string = std::format(
                            "{} {} {} {} {}",
                            state.last_decoded.last_frame_buffer_input_offset,
                            state.last_decoded.field_parity ? "ODD" : "EVEN",
                            offsets->field_start,
                            sample_label,
                            offsets->y);
                    if (offsets->cr && offsets->cb)
                        offset_string += std::format(" Cr {} Cb {}", *offsets->cr, *offsets->cb);
                    text_renderer.drawText(images.out_image, tl_x(10), vis_top + 34, offset_string, 1, command_buffer);
                    cursor_string += " " + offset_string;
                }
            }
        }
        if (state.paused)
            state.redo_last_field = true;
    }

    if (state.show_disc_code) {
        auto disc_info_strings = state.last_decoded.disc_info
                ? state.last_decoded.disc_info->asStrings()
                : std::vector<std::string>{"No disc info"};
        const int disc_scale = comp_scale(osd_base / 2);
        const int glyph_h = 24;
        const int bottom_margin = std::max(0, src.height - 955 - glyph_h * 2) / zoom;
        const int last_y = vis_top + vis_height - bottom_margin - glyph_h * disc_scale;
        const int line_step = glyph_h * disc_scale + 7;
        for (int i = (int)disc_info_strings.size() - 1, k = 0; i >= 0; i--, k++) {
            text_renderer.drawText(images.out_image, tl_x(90), last_y - k * line_step, disc_info_strings[i], disc_scale, command_buffer);
        }
        if (state.paused)
            state.redo_last_field = true;
    }

    return cursor_string;
}
