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

PictureFormat PictureFormat::ntsc(int width, int height, int first_column) {
    // 910 samples per line at 4 fsc, where square pixels take 780 (12 3/11 MHz)
    const double pixel_aspect = 780.0 / 910.0;
    const double samples_per_us = 315.0 / 88.0 * 4.0;
    const double line_us = 910.0 / samples_per_us;
    // The active line: after a blanking interval of 10.9 us, of which 1.5 us
    // (the front porch) precede the sync edge (RS-170A).  Measured on three
    // discs, the picture is centred within a fraction of a sample of this.
    const double center_us = (10.9 - 1.5) + (line_us - 10.9) / 2;
    // Sample s of the line is the pixel whose edges are s and s + 1
    const double center = center_us * samples_per_us - first_column + 0.5;
    const double picture_width = height * (4.0 / 3.0) / pixel_aspect;
    return {width, height, pixel_aspect, center - picture_width / 2, center + picture_width / 2, true};
}

PictureFormat PictureFormat::pal(int width, int height, int first_column) {
    // 1135 samples per line (line locked, 17.734375 MHz), where square
    // pixels take 944 (14.75 MHz)
    const double pixel_aspect = 944.0 / 1135.0;
    const double samples_per_us = 1135.0 / 64.0;
    // The active line: 52 us after a blanking interval of 12 us, of which
    // 1.5 us (the front porch) precede the sync edge (ITU-R BT.470 System B/G)
    const double center_us = (12.0 - 1.5) + 52.0 / 2;
    const double center = center_us * samples_per_us - first_column + 0.5;
    const double picture_width = height * (4.0 / 3.0) / pixel_aspect;
    return {width, height, pixel_aspect, center - picture_width / 2, center + picture_width / 2, true};
}

PictureFormat PictureFormat::muse(int width, int height) {
    return {width, height, (16.0 / 9.0) / ((double)width / height), 0.0, (double)width, false};
}

namespace {
    struct Rect {
        double x0, y0, x1, y1;
    };

    // The part of the image shown before the magnifier and the aspect mode
    // have their say
    Rect baseRect(const PictureFormat &f, bool full_image, double overscan) {
        if (full_image)
            return {0, 0, (double)f.width, (double)f.height};
        const double cx = (f.picture_x0 + f.picture_x1) / 2, cy = f.height / 2.0;
        const double hw = (f.picture_x1 - f.picture_x0) / 2 * (1 - overscan);
        const double hh = f.height / 2.0 * (1 - overscan);
        return {cx - hw, cy - hh, cx + hw, cy + hh};
    }
}

void defaultWindowSize(const PictureFormat &format, bool full_image, int &width, int &height) {
    const Rect r = baseRect(format, full_image, 0);
    const double w = r.x1 - r.x0, h = r.y1 - r.y0;
    const double aspect = w * format.pixel_aspect / h;
    width = std::max((int)std::ceil(w), (int)std::lround(h * aspect));
    height = std::max((int)std::ceil(h), (int)std::lround(width / aspect));
}

DisplayGeometry computeDisplayGeometry(const DisplayGeometryInput &in) {
    // The magnifier (Z key) picks a part of what is shown without it
    const Rect base = baseRect(in.format, in.full_image, in.overscan);
    const double zoom = in.zoom_factor;
    const double bw = base.x1 - base.x0, bh = base.y1 - base.y0;
    DisplayGeometry g;
    g.src_x0 = base.x0 + (in.zoom_cx - 0.5 / zoom) * bw;
    g.src_x1 = base.x0 + (in.zoom_cx + 0.5 / zoom) * bw;
    g.src_y0 = base.y0 + (in.zoom_cy - 0.5 / zoom) * bh;
    g.src_y1 = base.y0 + (in.zoom_cy + 0.5 / zoom) * bh;
    g.dst_x0 = 0;
    g.dst_y0 = 0;
    g.dst_x1 = in.dst_width;
    g.dst_y1 = in.dst_height;
    if (in.mode == AspectMode::eStretch)
        return g;

    // An anamorphic disc squeezes a 16:9 picture into the 4:3 frame
    const double source_pixel_aspect = in.format.pixel_aspect
            * (in.mode == AspectMode::eSqueeze && in.format.can_squeeze ? 4.0 / 3.0 : 1.0);
    // The shape of the shown part in display pixels
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
