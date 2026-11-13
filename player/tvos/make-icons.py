#!/usr/bin/env python3
# Copyright 2026 Staffan Ulfberg
# This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)
"""Draws the Apple TV app's icon and top-shelf images into Assets.xcassets.

    python3 player/tvos/make-icons.py [--preview <png>]

Everything is drawn here (numpy + PIL, the wordmark in the bundled Noto Sans),
so the pictures are changed by editing this file and running it again; the
PNGs it writes are checked in, and the build does not run it.  tvOS wants the
icon as a stack of layers that it moves against each other when the icon has
the focus: the background, the disc, the wordmark.  The sizes are Apple's:
the icon 400x240 (@1x and @2x) and 1280x768 for the store, the top-shelf
image 1920x720 and the wide one 2320x720 (both @1x and @2x).
"""

import argparse
import colorsys
import json
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = os.path.join(HERE, "..", "third_party", "noto", "NotoSansJP-Regular.ttf")
ASSETS = os.path.join(HERE, "Assets.xcassets")
BRAND = os.path.join(ASSETS, "App Icon & Top Shelf Image.brandassets")
INFO = {"author": "xcode", "version": 1}
SS = 3  # supersampling of the disc


def background(w, h, glow_x, glow_y):
    """Opaque: near-black blue, lighter around where the disc sits."""
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    d = np.hypot((x - glow_x * w) / h, (y - glow_y * h) / h)
    glow = np.exp(-(d / 0.75) ** 2)
    top = np.array([10, 14, 40], np.float32)
    lit = np.array([34, 48, 110], np.float32)
    img = top[None, None, :] * (1.0 - 0.45 * (y / h))[..., None] + (lit - top)[None, None, :] * glow[..., None]
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB").convert("RGBA")


def disc(diameter):
    """A laserdisc seen face on: the silver playing surface with its two rainbow
    reflections, the clear ring, the label and the spindle hole."""
    n = diameter * SS
    y, x = np.mgrid[0:n, 0:n].astype(np.float32)
    c = (n - 1) / 2.0
    r = np.hypot(x - c, y - c) / (n / 2.0)
    theta = np.arctan2(y - c, x - c)

    theta0 = np.deg2rad(-52.0)  # where the light comes from
    wedge = np.maximum(0.0, np.cos(theta - theta0)) ** 7 + 0.8 * np.maximum(0.0, -np.cos(theta - theta0)) ** 7
    # The colours run across each reflection and shift slowly with the radius
    hue = (0.62 - 0.55 * np.sin(theta - theta0) * np.sign(np.cos(theta - theta0)) * 1.6 + 0.25 * r) % 1.0
    hsv = np.vectorize(colorsys.hsv_to_rgb)
    rr, gg, bb = hsv(hue, 0.80, 1.0)
    rainbow = np.stack([rr, gg, bb], axis=-1).astype(np.float32)

    # Silver, brighter towards the light, with faint bands where the programme
    # areas of a side meet
    silver = 0.50 + 0.16 * np.cos(theta - theta0) ** 2 + 0.035 * np.sin(r * 46.0) + 0.02 * np.sin(r * 173.0)
    surface = silver[..., None] * np.array([0.86, 0.90, 1.0], np.float32)
    surface = surface * (1.0 - 0.82 * wedge[..., None]) + rainbow * (0.95 * wedge[..., None])
    streak = np.maximum(0.0, np.cos(theta - theta0 - 0.05)) ** 90
    surface = surface + 0.55 * streak[..., None]

    rgb = np.zeros((n, n, 3), np.float32)
    alpha = np.zeros((n, n), np.float32)

    def ring(lo, hi, colour, a=1.0):
        m = (r >= lo) & (r < hi)
        rgb[m] = colour[m] if isinstance(colour, np.ndarray) and colour.ndim == 3 else colour
        alpha[m] = a

    ring(0.385, 0.985, surface)
    ring(0.985, 1.0, np.array([0.80, 0.84, 0.92], np.float32))            # the rim
    ring(0.345, 0.385, np.array([0.34, 0.37, 0.46], np.float32), 0.92)    # the clear ring
    label = (0.11 + 0.05 * np.cos(theta - theta0))[..., None] * np.array([1.0, 1.15, 2.2], np.float32)
    ring(0.10, 0.345, label)
    ring(0.325, 0.335, np.array([0.86, 0.70, 0.36], np.float32))          # a gold line on the label
    ring(0.10, 0.118, np.array([0.70, 0.73, 0.80], np.float32))           # the hub
    # r < 0.10: the spindle hole, open

    out = np.dstack([np.clip(rgb, 0, 1), alpha])
    img = Image.fromarray((out * 255).astype(np.uint8), "RGBA")
    return img.resize((diameter, diameter), Image.LANCZOS)


def disc_layer(w, h, cx, cy, diameter):
    """Transparent but for the disc and its shadow."""
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = int(round(diameter * h))
    x0, y0 = int(round(cx * w - d / 2)), int(round(cy * h - d / 2))
    shadow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    off = int(0.03 * h)
    ImageDraw.Draw(shadow).ellipse([x0 + off, y0 + off, x0 + d + off, y0 + d + off], fill=(0, 0, 0, 150))
    layer = Image.alpha_composite(layer, shadow.filter(ImageFilter.GaussianBlur(0.03 * h)))
    layer.alpha_composite(disc(d), (x0, y0))
    return layer


def text_layer(w, h, x, y, size, tagline=None):
    """The wordmark, its left edge at x and its middle at y (fractions), the
    letters `size` of the height tall; an optional line of small text under it."""
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    font = ImageFont.truetype(FONT, int(size * h))
    stroke = max(1, int(size * h * 0.022))  # the regular weight made a little heavier
    tx, ty = int(x * w), int(y * h)

    shadow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).text((tx, ty + int(0.012 * h)), "museld", font=font, fill=(0, 0, 0, 200), anchor="lm",
                                stroke_width=stroke, stroke_fill=(0, 0, 0, 200))
    layer = Image.alpha_composite(layer, shadow.filter(ImageFilter.GaussianBlur(0.012 * h)))
    draw = ImageDraw.Draw(layer)
    draw.text((tx, ty), "museld", font=font, fill=(244, 246, 252, 255), anchor="lm",
              stroke_width=stroke, stroke_fill=(244, 246, 252, 255))
    if tagline:
        small = ImageFont.truetype(FONT, int(size * h * 0.27))
        draw.text((tx + int(0.004 * w), ty + int(size * h * 0.78)), tagline, font=small,
                  fill=(170, 182, 222, 255), anchor="lm")
    return layer


# The icon: the disc on the left, cut by the edges, the wordmark beside it
def icon_layers(w, h):
    return [("Front", text_layer(w, h, 0.565, 0.50, 0.19)),
            ("Middle", disc_layer(w, h, 0.14, 0.50, 1.22)),
            ("Back", background(w, h, 0.14, 0.50))]


def shelf(w, h):
    img = background(w, h, 0.21, 0.50)
    img = Image.alpha_composite(img, disc_layer(w, h, 0.21, 0.52, 1.36))
    img = Image.alpha_composite(img, text_layer(w, h, 0.49, 0.44, 0.24,
                                                tagline="MUSE, NTSC and PAL laserdisc player"))
    return img.convert("RGB")


def flatten(layers):
    img = None
    for _, layer in reversed(layers):
        img = layer if img is None else Image.alpha_composite(img, layer)
    return img.convert("RGB")


def write_json(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(obj, f, indent=2)
        f.write("\n")


def write_stack(name, w, h, scales):
    stack = os.path.join(BRAND, name + ".imagestack")
    names = [n for n, _ in icon_layers(8, 8)]
    write_json(os.path.join(stack, "Contents.json"),
               {"info": INFO, "layers": [{"filename": n + ".imagestacklayer"} for n in names]})
    for n in names:
        write_json(os.path.join(stack, n + ".imagestacklayer", "Contents.json"), {"info": INFO})
    images = {n: [] for n in names}
    for scale in scales:
        for n, layer in icon_layers(w * scale, h * scale):
            filename = f"{n.lower()}@{scale}x.png" if scale > 1 else f"{n.lower()}.png"
            path = os.path.join(stack, n + ".imagestacklayer", "Content.imageset", filename)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            # The background must be opaque; the other layers keep their alpha
            (layer.convert("RGB") if n == "Back" else layer).save(path, optimize=True)
            images[n].append({"filename": filename, "idiom": "tv", "scale": f"{scale}x"})
    for n in names:
        write_json(os.path.join(stack, n + ".imagestacklayer", "Content.imageset", "Contents.json"),
                   {"images": images[n], "info": INFO})


def write_shelf(name, w, h):
    imageset = os.path.join(BRAND, name + ".imageset")
    images = []
    for scale in (1, 2):
        filename = f"shelf@{scale}x.png" if scale > 1 else "shelf.png"
        os.makedirs(imageset, exist_ok=True)
        shelf(w * scale, h * scale).save(os.path.join(imageset, filename), optimize=True)
        images.append({"filename": filename, "idiom": "tv", "scale": f"{scale}x"})
    write_json(os.path.join(imageset, "Contents.json"), {"images": images, "info": INFO})


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--preview", metavar="PNG",
                        help="only write the flattened icon above the top-shelf image to this file")
    args = parser.parse_args()
    if args.preview:
        icon = flatten(icon_layers(800, 480))
        top = shelf(1920, 720)
        sheet = Image.new("RGB", (1920, 480 + 40 + 720), (40, 40, 40))
        sheet.paste(icon, (560, 0))
        sheet.paste(top, (0, 520))
        sheet.save(args.preview)
        return
    write_json(os.path.join(ASSETS, "Contents.json"), {"info": INFO})
    write_json(os.path.join(BRAND, "Contents.json"), {"info": INFO, "assets": [
        {"filename": "App Icon - App Store.imagestack", "idiom": "tv", "role": "primary-app-icon", "size": "1280x768"},
        {"filename": "App Icon.imagestack", "idiom": "tv", "role": "primary-app-icon", "size": "400x240"},
        {"filename": "Top Shelf Image Wide.imageset", "idiom": "tv", "role": "top-shelf-image-wide", "size": "2320x720"},
        {"filename": "Top Shelf Image.imageset", "idiom": "tv", "role": "top-shelf-image", "size": "1920x720"},
    ]})
    write_stack("App Icon", 400, 240, (1, 2))
    write_stack("App Icon - App Store", 1280, 768, (1,))
    write_shelf("Top Shelf Image", 1920, 720)
    write_shelf("Top Shelf Image Wide", 2320, 720)


if __name__ == "__main__":
    main()
