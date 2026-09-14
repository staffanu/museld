// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "DisplayGeometry.h"

#include <algorithm>
#include <cmath>

const char *aspectModeName(AspectMode mode) {
    switch (mode) {
        case AspectMode::eNormal: return "NORMAL";
        case AspectMode::eZoom: return "ZOOM";
        case AspectMode::eSqueeze: return "SQUEEZE";
        case AspectMode::eStretch: return "STRETCH";
    }
    return "?";
}

DisplayGeometry computeDisplayGeometry(const DisplayGeometryInput &in) {
    // The magnifier (Z key) picks the part of the decoded image to show
    const double zoom = in.zoom_factor;
    DisplayGeometry g;
    g.src_x0 = (in.zoom_cx - 0.5 / zoom) * in.src_width;
    g.src_x1 = (in.zoom_cx + 0.5 / zoom) * in.src_width;
    g.src_y0 = (in.zoom_cy - 0.5 / zoom) * in.src_height;
    g.src_y1 = (in.zoom_cy + 0.5 / zoom) * in.src_height;
    g.dst_x0 = 0;
    g.dst_y0 = 0;
    g.dst_x1 = in.dst_width;
    g.dst_y1 = in.dst_height;
    if (in.mode == AspectMode::eStretch)
        return g;

    // Squeeze only means something for a 4:3 frame
    const double picture_aspect = in.mode == AspectMode::eSqueeze && in.source_aspect < 1.5
                                  ? 16.0 / 9.0 : in.source_aspect;
    // Shape of one decoded pixel on a square-pixel display, then the shape
    // of the shown part in display pixels
    const double source_pixel_aspect = picture_aspect / ((double)in.src_width / in.src_height);
    const double shown_aspect = (g.src_x1 - g.src_x0) * source_pixel_aspect / (g.src_y1 - g.src_y0)
                                / in.display_pixel_aspect;
    const double window_aspect = (double)in.dst_width / in.dst_height;

    if (in.mode == AspectMode::eZoom) {
        // Cover: keep the destination, crop the source dimension that overflows
        if (shown_aspect > window_aspect) {
            const double keep = window_aspect / shown_aspect;
            const double cut = (g.src_x1 - g.src_x0) * (1 - keep) / 2;
            g.src_x0 += cut;
            g.src_x1 -= cut;
        } else {
            const double keep = shown_aspect / window_aspect;
            const double cut = (g.src_y1 - g.src_y0) * (1 - keep) / 2;
            g.src_y0 += cut;
            g.src_y1 -= cut;
        }
        return g;
    }

    // Contain: keep the source, shrink the destination dimension that would overflow
    int w = in.dst_width, h = in.dst_height;
    if (shown_aspect > window_aspect)
        h = std::max(1, (int)std::lround(in.dst_width / shown_aspect));
    else
        w = std::max(1, (int)std::lround(in.dst_height * shown_aspect));
    g.dst_x0 = (in.dst_width - w) / 2;
    g.dst_y0 = (in.dst_height - h) / 2;
    g.dst_x1 = g.dst_x0 + w;
    g.dst_y1 = g.dst_y0 + h;
    return g;
}
