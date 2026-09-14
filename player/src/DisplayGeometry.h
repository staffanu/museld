// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_DISPLAYGEOMETRY_H
#define MUSECPP_DISPLAYGEOMETRY_H

// How the decoded picture is fitted to the window.  The decoded image has
// non-square pixels (MUSE: 1122x1032 for a 16:9 picture, NTSC: 764x480 for
// 4:3), and so may the display, so the picture is always resampled to its
// intended shape unless STRETCH is asked for.
enum class AspectMode {
    eNormal,   // the picture at its intended aspect, bars where the window is wider or taller
    eZoom,     // fill the window, cropping the overflow: shows a letterboxed film full-width
    eSqueeze,  // anamorphic disc: the 4:3 frame holds a 16:9 picture squeezed horizontally
    eStretch,  // fill the window, ignoring the aspect
};

const char *aspectModeName(AspectMode mode);

struct DisplayGeometryInput {
    AspectMode mode;
    double source_aspect;         // the picture's intended width over height (16:9 MUSE, 4:3 NTSC)
    int src_width, src_height;    // the decoded image
    int zoom_factor;              // the magnifier: 1, 2, 4
    double zoom_cx, zoom_cy;      // its center, as fractions of the decoded image
    double display_pixel_aspect;  // the monitor's pixel shape, width over height
    int dst_width, dst_height;    // the swap chain image
};

// What to blit where: the source rectangle in decoded-image pixels, and the
// destination rectangle in swap chain pixels
struct DisplayGeometry {
    double src_x0, src_y0, src_x1, src_y1;
    int dst_x0, dst_y0, dst_x1, dst_y1;
};

DisplayGeometry computeDisplayGeometry(const DisplayGeometryInput &in);

#endif //MUSECPP_DISPLAYGEOMETRY_H
