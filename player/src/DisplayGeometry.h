// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_DISPLAYGEOMETRY_H
#define MUSECPP_DISPLAYGEOMETRY_H

// The geometry of a decoder's output image.  Its pixels are not square, and
// for NTSC the image is wider than the picture: the 764 columns are the
// BT.601 digital active line (720 samples at 13.5 MHz, from 122 samples after
// the sync edge) at 4 fsc, which was made wider than the analog picture so
// that the blanking edges, with their tolerances, fall inside it.  The
// columns that make up the standard 4:3 picture are the middle ~747; the rest
// is blanking margin, black on most discs, which a TV hides in its overscan.
struct PictureFormat {
    int width, height;              // the decoded image
    double pixel_aspect;            // width over height of one of its pixels on a square-pixel display
    double picture_x0, picture_x1;  // the columns [x0, x1) of the standard picture, in pixel-edge coordinates
    bool can_squeeze;               // a 4:3 frame, which may hold an anamorphic 16:9 picture

    // first_column: the 4 fsc sample after the sync edge that column 0 holds
    static PictureFormat ntsc(int width, int height, int first_column);
    static PictureFormat pal(int width, int height, int first_column);
    static PictureFormat muse(int width, int height);
};

// How the picture is fitted to the window.  It is always resampled to its
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
    PictureFormat format;
    bool full_image;              // show the whole decoded image, blanking margins included
    double overscan;              // else: the fraction of the standard picture hidden in each
                                  // dimension, half on either side (0.05 shows the central 95 %)
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

// A window of the picture's shape that shows it without downscaling in
// either direction
void defaultWindowSize(const PictureFormat &format, bool full_image, int &width, int &height);

#endif //MUSECPP_DISPLAYGEOMETRY_H
