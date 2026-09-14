// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "DisplayGeometry.h"

using Catch::Matchers::WithinAbs;

namespace {
    constexpr int NTSC_W = 764, NTSC_H = 480;
    constexpr int MUSE_W = 1122, MUSE_H = 1032;

    DisplayGeometryInput ntsc(AspectMode mode, int dst_w, int dst_h, double display_par = 1.0) {
        return {mode, 4.0 / 3.0, NTSC_W, NTSC_H, 1, 0.5, 0.5, display_par, dst_w, dst_h};
    }
    DisplayGeometryInput muse(AspectMode mode, int dst_w, int dst_h, double display_par = 1.0) {
        return {mode, 16.0 / 9.0, MUSE_W, MUSE_H, 1, 0.5, 0.5, display_par, dst_w, dst_h};
    }
    void expectWholeSource(const DisplayGeometry &g, int w, int h) {
        CHECK(g.src_x0 == 0);
        CHECK(g.src_y0 == 0);
        CHECK(g.src_x1 == w);
        CHECK(g.src_y1 == h);
    }
    void expectWholeDest(const DisplayGeometry &g, int w, int h) {
        CHECK(g.dst_x0 == 0);
        CHECK(g.dst_y0 == 0);
        CHECK(g.dst_x1 == w);
        CHECK(g.dst_y1 == h);
    }
}

TEST_CASE("NTSC on a 16:9 screen: normal pillarboxes to 4:3", "[display]") {
    const auto g = computeDisplayGeometry(ntsc(AspectMode::eNormal, 1920, 1080));
    expectWholeSource(g, NTSC_W, NTSC_H);
    CHECK(g.dst_y0 == 0);
    CHECK(g.dst_y1 == 1080);
    CHECK(g.dst_x0 == 240);
    CHECK(g.dst_x1 == 1680);
}

TEST_CASE("NTSC on a 16:9 screen: zoom fills the width and crops 12.5% top and bottom", "[display]") {
    const auto g = computeDisplayGeometry(ntsc(AspectMode::eZoom, 1920, 1080));
    expectWholeDest(g, 1920, 1080);
    CHECK(g.src_x0 == 0);
    CHECK(g.src_x1 == NTSC_W);
    CHECK_THAT(g.src_y0, WithinAbs(60.0, 1e-9));
    CHECK_THAT(g.src_y1, WithinAbs(420.0, 1e-9));
}

TEST_CASE("NTSC on a 16:9 screen: squeeze shows the anamorphic frame as 16:9, and stretch fills", "[display]") {
    const auto s = computeDisplayGeometry(ntsc(AspectMode::eSqueeze, 1920, 1080));
    expectWholeSource(s, NTSC_W, NTSC_H);
    expectWholeDest(s, 1920, 1080);
    const auto t = computeDisplayGeometry(ntsc(AspectMode::eStretch, 1000, 100));
    expectWholeSource(t, NTSC_W, NTSC_H);
    expectWholeDest(t, 1000, 100);
}

TEST_CASE("NTSC in its own aspect-correct window is shown edge to edge", "[display]") {
    const auto g = computeDisplayGeometry(ntsc(AspectMode::eNormal, 764, 573));
    expectWholeSource(g, NTSC_W, NTSC_H);
    CHECK(g.dst_x0 == 0);
    CHECK(g.dst_y0 == 0);
    CHECK(g.dst_x1 == 764);
    CHECK(g.dst_y1 == 573);
}

TEST_CASE("MUSE on a 16:9 screen fills it; squeeze is a no-op for a 16:9 source", "[display]") {
    for (auto mode : {AspectMode::eNormal, AspectMode::eZoom, AspectMode::eSqueeze}) {
        const auto g = computeDisplayGeometry(muse(mode, 1920, 1080));
        expectWholeSource(g, MUSE_W, MUSE_H);
        expectWholeDest(g, 1920, 1080);
    }
}

TEST_CASE("MUSE on a 4:3 screen: normal letterboxes, zoom crops the sides", "[display]") {
    const auto n = computeDisplayGeometry(muse(AspectMode::eNormal, 1600, 1200));
    expectWholeSource(n, MUSE_W, MUSE_H);
    CHECK(n.dst_x0 == 0);
    CHECK(n.dst_x1 == 1600);
    CHECK(n.dst_y0 == 150);
    CHECK(n.dst_y1 == 1050);
    const auto z = computeDisplayGeometry(muse(AspectMode::eZoom, 1600, 1200));
    expectWholeDest(z, 1600, 1200);
    CHECK(z.src_y0 == 0);
    CHECK(z.src_y1 == MUSE_H);
    CHECK_THAT(z.src_x0, WithinAbs(MUSE_W * 0.125, 1e-9));
    CHECK_THAT(z.src_x1, WithinAbs(MUSE_W * 0.875, 1e-9));
}

TEST_CASE("Non-square display pixels: 1440x1080 on a 16:9 panel shows 4:3 as a 1080-wide square", "[display]") {
    const auto g = computeDisplayGeometry(ntsc(AspectMode::eNormal, 1440, 1080, 4.0 / 3.0));
    CHECK(g.dst_y0 == 0);
    CHECK(g.dst_y1 == 1080);
    CHECK(g.dst_x0 == 180);
    CHECK(g.dst_x1 == 1260);
}

TEST_CASE("The magnifier picks the source rectangle, the aspect fitting applies to it", "[display]") {
    DisplayGeometryInput in = ntsc(AspectMode::eNormal, 1920, 1080);
    in.zoom_factor = 2;
    in.zoom_cx = 0.25;
    in.zoom_cy = 0.5;
    const auto g = computeDisplayGeometry(in);
    CHECK(g.src_x0 == 0);
    CHECK(g.src_x1 == NTSC_W / 2);
    CHECK(g.src_y0 == NTSC_H / 4);
    CHECK(g.src_y1 == NTSC_H * 3 / 4);
    CHECK(g.dst_x0 == 240); // still 4:3 on screen
    CHECK(g.dst_x1 == 1680);
}
