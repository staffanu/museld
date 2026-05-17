#include "OsdOverlay.h"

#include <format>
#include <vector>
#include <GLFW/glfw3.h>

#include "Decoder.h"
#include "PlayerState.h"
#include "ResultImages.h"
#include "TextRenderer.h"
#include "DiscInfo.h"
#include "musevk/CommandBuffer.h"

std::string OsdOverlay::render(musevk::CommandBuffer &command_buffer,
                               ResultImages &images,
                               PlayerState &state,
                               const Decoder &decoder,
                               GLFWwindow *window,
                               TextRenderer &text_renderer) {
    std::string cursor_string;
    if (!state.osd_text.empty()) {
        if (state.paused && state.osd_text_remaining_frames)
            state.redo_last_field = true;
        state.osd_text_remaining_frames = 100;
        state.displayed_osd_text = state.osd_text;
        state.osd_text.clear();
    }
    if (state.osd_text_remaining_frames) {
        text_renderer.drawText(images.out_image, 90, 50, state.displayed_osd_text, 4, command_buffer);
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
            const auto src = decoder.getSourceDimensions();
            double rel_x = (xpos / xsize - 0.5) / state.zoom_factor + state.zoom_center.first;
            double rel_y = (ypos / ysize - 0.5) / state.zoom_factor + state.zoom_center.second;
            int field_x = (int)(rel_x * src.field_width);
            int field_y = (int)(rel_y * src.field_height);
            cursor_string = std::format("({}, {}) ({}, {})",
                                            (int)(rel_x * src.width),
                                            (int)(rel_y * src.height),
                                            field_x, field_y);
            text_renderer.drawText(images.out_image, 10, 8, cursor_string, 1, command_buffer);
            if (state.field_interpolation_mode == Decoder::FieldInterpolationMode::eForceIntraField && state.paused) {
                if (auto offsets = decoder.computePixelFileOffsets(
                            field_x, field_y, state.last_decoded.field_parity,
                            state.last_decoded.last_frame_buffer_input_offset,
                            state.last_decoded.input_samples_per_muse_sample)) {
                    std::string offset_string = std::format(
                            "{} {} {} Y {} Cr {} Cb {}",
                            state.last_decoded.last_frame_buffer_input_offset,
                            state.last_decoded.field_parity ? "ODD" : "EVEN",
                            offsets->field_start,
                            offsets->y, offsets->cr, offsets->cb);
                    text_renderer.drawText(images.out_image, 10, 34, offset_string, 1, command_buffer);
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
        for (int i = (int)disc_info_strings.size() - 1, y = 955; i >= 0; i--, y -= 55) {
            text_renderer.drawText(images.out_image, 90, y, disc_info_strings[i], 2, command_buffer);
        }
        if (state.paused)
            state.redo_last_field = true;
    }

    return cursor_string;
}
