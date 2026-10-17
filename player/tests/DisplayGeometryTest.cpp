// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "DisplayGeometry.h"

using Catch::Matchers::WithinAbs;

namespace {
    const PictureFormat c_ntsc = PictureFormat::ntsc(764, 480, 129);
    const PictureFormat c_muse = PictureFormat::muse(1122, 1032);

    DisplayGeometryInput ntsc(AspectMode mode, int dst_w, int dst_h, double display_par = 1.0) {
        return {mode, c_ntsc, false, 0.0, 1, 0.5, 0.5, display_par, dst_w, dst_h};
    }
    DisplayGeometryInput muse(AspectMode mode, int dst_w, int dst_h, double display_par = 1.0) {
        return {mode, c_muse, false, 0.0, 1, 0.5, 0.5, display_par, dst_w, dst_h};
    }
    void expectNtscPicture(const DisplayGeometry &g) {
        CHECK_THAT(g.src_x0, WithinAbs(c_ntsc.picture_x0, 1e-9));
        CHECK_THAT(g.src_x1, WithinAbs(c_ntsc.picture_x1, 1e-9));
        CHECK(g.src_y0 == 0);
        CHECK(g.src_y1 == 480);
    }
    void expectWholeDest(const DisplayGeometry &g, int w, int h) {
        CHECK(g.dst_x0 == 0);
        CHECK(g.dst_y0 == 0);
        CHECK(g.dst_x1 == w);
        CHECK(g.dst_y1 == h);
    }
}

TEST_CASE("The NTSC picture format: 6/7 pixels, the 4:3 picture in the middle ~747 columns", "[display]") {
    CHECK_THAT(c_ntsc.pixel_aspect, WithinAbs(6.0 / 7.0, 1e-12));
    CHECK(c_ntsc.can_squeeze);
    const double w = c_ntsc.picture_x1 - c_ntsc.picture_x0;
    CHECK_THAT(w, WithinAbs(746.667, 1e-3));
    CHECK_THAT(w * c_ntsc.pixel_aspect / c_ntsc.height, WithinAbs(4.0 / 3.0, 1e-12));
    // centred on the standard's active line, a column right of the image centre,
    // leaving the blanking margins (measured: 7-8 columns left, 5-6 right) outside
    CHECK_THAT((c_ntsc.picture_x0 + c_ntsc.picture_x1) / 2, WithinAbs(383.06, 0.05));
    CHECK(c_ntsc.picture_x0 > 9.0);
    CHECK(c_ntsc.picture_x1 < 757.0);
}

TEST_CASE("The PAL picture format: 944/1135 pixels, the 4:3 picture in the middle ~923 columns", "[display]") {
    const PictureFormat pal = PictureFormat::pal(944, 576, 180);
    CHECK_THAT(pal.pixel_aspect, WithinAbs(944.0 / 1135.0, 1e-12));
    CHECK(pal.can_squeeze);
    const double w = pal.picture_x1 - pal.picture_x0;
    CHECK_THAT(w, WithinAbs(923.39, 1e-2));
    CHECK_THAT(w * pal.pixel_aspect / pal.height, WithinAbs(4.0 / 3.0, 1e-12));
    // the 52 us active line starts 10.5 us after the sync edge: column 186.2
    // of the 1135-sample line, 6.2 columns into the image
    CHECK_THAT(pal.picture_x0, WithinAbs(6.1, 0.1));
    CHECK(pal.picture_x1 < 944.0);
    CHECK_THAT((pal.picture_x0 + pal.picture_x1) / 2, WithinAbs(467.8, 0.1));
}

TEST_CASE("NTSC on a 16:9 screen: the 4:3 picture, pillarboxed", "[display]") {
    const auto g = computeDisplayGeometry(ntsc(AspectMode::eNormal, 1920, 1080));
    expectNtscPicture(g);
    CHECK(g.dst_y0 == 0);
    CHECK(g.dst_y1 == 1080);
    CHECK(g.dst_x0 == 240);
    CHECK(g.dst_x1 == 1680);
}

TEST_CASE("NTSC full image: all 764 columns, a little wider than 4:3", "[display]") {
    DisplayGeometryInput in = ntsc(AspectMode::eNormal, 1920, 1080);
    in.full_image = true;
    const auto g = computeDisplayGeometry(in);
    CHECK(g.src_x0 == 0);
    CHECK(g.src_x1 == 764);
    CHECK(g.src_y0 == 0);
    CHECK(g.src_y1 == 480);
    CHECK(g.dst_x1 - g.dst_x0 == 1473); // 1080 * 764 * (6/7) / 480
    CHECK(g.dst_x0 == 223);
}

TEST_CASE("Overscan hides the same fraction in both dimensions and keeps the shape", "[display]") {
    DisplayGeometryInput in = ntsc(AspectMode::eNormal, 1920, 1080);
    in.overscan = 0.05;
    const auto g = computeDisplayGeometry(in);
    const double w = c_ntsc.picture_x1 - c_ntsc.picture_x0;
    CHECK_THAT(g.src_x1 - g.src_x0, WithinAbs(0.95 * w, 1e-9));
    CHECK_THAT((g.src_x0 + g.src_x1) / 2, WithinAbs((c_ntsc.picture_x0 + c_ntsc.picture_x1) / 2, 1e-9));
    CHECK_THAT(g.src_y0, WithinAbs(12.0, 1e-9));
    CHECK_THAT(g.src_y1, WithinAbs(468.0, 1e-9));
    CHECK(g.dst_x0 == 240);
    CHECK(g.dst_x1 == 1680);
}

TEST_CASE("NTSC on a 16:9 screen: zoom fills the width and crops 12.5% top and bottom", "[display]") {
    const auto g = computeDisplayGeometry(ntsc(AspectMode::eZoom, 1920, 1080));
    expectWholeDest(g, 1920, 1080);
    CHECK_THAT(g.src_x0, WithinAbs(c_ntsc.picture_x0, 1e-9));
    CHECK_THAT(g.src_x1, WithinAbs(c_ntsc.picture_x1, 1e-9));
    CHECK_THAT(g.src_y0, WithinAbs(60.0, 1e-9));
    CHECK_THAT(g.src_y1, WithinAbs(420.0, 1e-9));
}

TEST_CASE("NTSC on a 16:9 screen: squeeze shows the anamorphic frame as 16:9, and stretch fills", "[display]") {
    const auto s = computeDisplayGeometry(ntsc(AspectMode::eSqueeze, 1920, 1080));
    expectNtscPicture(s);
    expectWholeDest(s, 1920, 1080);
    const auto t = computeDisplayGeometry(ntsc(AspectMode::eStretch, 1000, 100));
    expectNtscPicture(t);
    expectWholeDest(t, 1000, 100);
}

TEST_CASE("Default windows show the picture edge to edge without downscaling", "[display]") {
    int w, h;
    defaultWindowSize(c_ntsc, false, w, h);
    CHECK(w == 747);
    CHECK(h == 560);
    const auto g = computeDisplayGeometry(ntsc(AspectMode::eNormal, w, h));
    expectWholeDest(g, w, h);
    defaultWindowSize(c_ntsc, true, w, h);
    CHECK(w == 764);
    CHECK(h == 560);
    defaultWindowSize(c_muse, false, w, h);
    CHECK(w == 1835);
    CHECK(h == 1032);
}

TEST_CASE("MUSE on a 16:9 screen fills it; squeeze is a no-op, and the full image is the picture", "[display]") {
    for (auto mode : {AspectMode::eNormal, AspectMode::eZoom, AspectMode::eSqueeze}) {
        for (bool full : {false, true}) {
            DisplayGeometryInput in = muse(mode, 1920, 1080);
            in.full_image = full;
            const auto g = computeDisplayGeometry(in);
            CHECK_THAT(g.src_x0, WithinAbs(0.0, 1e-9));
            CHECK_THAT(g.src_x1, WithinAbs(1122.0, 1e-9));
            CHECK_THAT(g.src_y0, WithinAbs(0.0, 1e-9));
            CHECK_THAT(g.src_y1, WithinAbs(1032.0, 1e-9));
            expectWholeDest(g, 1920, 1080);
        }
    }
}

TEST_CASE("MUSE on a 4:3 screen: normal letterboxes, zoom crops the sides", "[display]") {
    const auto n = computeDisplayGeometry(muse(AspectMode::eNormal, 1600, 1200));
    CHECK(n.dst_x0 == 0);
    CHECK(n.dst_x1 == 1600);
    CHECK(n.dst_y0 == 150);
    CHECK(n.dst_y1 == 1050);
    const auto z = computeDisplayGeometry(muse(AspectMode::eZoom, 1600, 1200));
    expectWholeDest(z, 1600, 1200);
    CHECK(z.src_y0 == 0);
    CHECK(z.src_y1 == 1032);
    CHECK_THAT(z.src_x0, WithinAbs(1122 * 0.125, 1e-9));
    CHECK_THAT(z.src_x1, WithinAbs(1122 * 0.875, 1e-9));
}

TEST_CASE("Non-square display pixels: 1440x1080 on a 16:9 panel shows 4:3 as a 1080-wide square", "[display]") {
    const auto g = computeDisplayGeometry(ntsc(AspectMode::eNormal, 1440, 1080, 4.0 / 3.0));
    CHECK(g.dst_y0 == 0);
    CHECK(g.dst_y1 == 1080);
    CHECK(g.dst_x0 == 180);
    CHECK(g.dst_x1 == 1260);
}

TEST_CASE("The magnifier picks a part of the shown picture, the aspect fitting applies to it", "[display]") {
    DisplayGeometryInput in = ntsc(AspectMode::eNormal, 1920, 1080);
    in.zoom_factor = 2;
    in.zoom_cx = 0.25;
    in.zoom_cy = 0.5;
    const auto g = computeDisplayGeometry(in);
    CHECK_THAT(g.src_x0, WithinAbs(c_ntsc.picture_x0, 1e-9));
    CHECK_THAT(g.src_x1, WithinAbs((c_ntsc.picture_x0 + c_ntsc.picture_x1) / 2, 1e-9));
    CHECK_THAT(g.src_y0, WithinAbs(120.0, 1e-9));
    CHECK_THAT(g.src_y1, WithinAbs(360.0, 1e-9));
    CHECK(g.dst_x0 == 240); // still 4:3 on screen
    CHECK(g.dst_x1 == 1680);
}
