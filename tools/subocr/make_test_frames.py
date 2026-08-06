#!/usr/bin/env python3
"""Generate synthetic MUSE-like frames with burned-in Japanese subtitles.

Produces a directory of PNG frames (sampled at FPS below) plus ground_truth.json
with the cue text and timing, for scoring the OCR pipeline in subocr.py.
The look mimics laserdisc subtitles: white glyphs with a black outline,
bottom-centered, over moving photographic-ish backgrounds, with analog softness
(blur + grain) applied after compositing.
"""

import json
import math
import random
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 1920, 1035
FPS = 3.0
DURATION = 90.0
FONT_PATH = Path(__file__).resolve().parents[2] / "player/third_party/noto/NotoSansJP-Regular.ttf"

CUES = [
    (2.0, 6.5, "こんな夜に出歩くなんて危険だ"),
    (8.0, 12.0, "「約束は必ず守る」と彼は言った"),
    (14.5, 19.0, "明日の朝８時に東京駅で会いましょう"),
    (21.0, 26.5, "この街には もう戻れないかもしれない\nそれでも行くしかないんだ"),
    (30.0, 34.0, "レーザーディスクの画質は素晴らしい"),
    (37.0, 41.5, "何を言っているのか分からないわ"),
    (44.0, 49.0, "システムの起動には約３０秒かかります"),
    (52.0, 57.0, "静かに…誰かが来る"),
    (60.0, 65.0, "昔々、あるところに小さな村がありました"),
    (68.0, 73.5, "これで終わりだと思うなよ！"),
    (77.0, 82.0, "音声と映像の同期は完璧です"),
]


def make_scene(rng):
    """A scene is a background image larger than the frame, panned over time."""
    bw, bh = W + 400, H + 200
    base = Image.new("RGB", (bw, bh))
    d = ImageDraw.Draw(base)
    # gradient background
    top = tuple(rng.randint(0, 255) for _ in range(3))
    bot = tuple(rng.randint(0, 255) for _ in range(3))
    for y in range(bh):
        t = y / bh
        d.line([(0, y), (bw, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(top, bot)))
    # random shapes to give the OCR detector something to trip on
    for _ in range(rng.randint(8, 20)):
        x0, y0 = rng.randint(0, bw), rng.randint(0, bh)
        x1, y1 = x0 + rng.randint(40, 600), y0 + rng.randint(40, 600)
        col = tuple(rng.randint(0, 255) for _ in range(3))
        if rng.random() < 0.5:
            d.ellipse([x0, y0, x1, y1], fill=col)
        else:
            d.rectangle([x0, y0, x1, y1], fill=col)
    # one scene in three is deliberately bright behind the subtitle band
    if rng.random() < 0.33:
        d.rectangle([0, bh - 300, bw, bh], fill=(rng.randint(180, 240),) * 3)
    return base.filter(ImageFilter.GaussianBlur(3))


def cue_at(t):
    for start, end, text in CUES:
        if start <= t < end:
            return text
    return None


def main(outdir):
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    rng = random.Random(1035)
    font = ImageFont.truetype(str(FONT_PATH), 58)

    scene, scene_end = None, -1.0
    n_frames = int(DURATION * FPS)
    for i in range(n_frames):
        t = i / FPS
        if t >= scene_end:
            scene = make_scene(rng)
            scene_end = t + rng.uniform(8.0, 14.0)
            panx0, pany0 = rng.uniform(0, 400), rng.uniform(0, 200)
            panvx, panvy = rng.uniform(-15, 15), rng.uniform(-8, 8)
            scene_t0 = t
        dt = t - scene_t0
        px = min(max(panx0 + panvx * dt, 0), 400)
        py = min(max(pany0 + panvy * dt, 0), 200)
        frame = scene.crop((int(px), int(py), int(px) + W, int(py) + H))

        text = cue_at(t)
        if text:
            d = ImageDraw.Draw(frame)
            lines = text.split("\n")
            line_h = 74
            y = H - 60 - line_h * len(lines)
            for line in lines:
                tw = d.textlength(line, font=font)
                d.text(((W - tw) / 2, y), line, font=font,
                       fill=(245, 245, 245), stroke_width=4, stroke_fill=(10, 10, 10))
                y += line_h

        # analog softness + grain
        frame = frame.filter(ImageFilter.GaussianBlur(0.8))
        arr = np.asarray(frame, dtype=np.int16)
        noise = np.random.default_rng(i).normal(0, 5, arr.shape)
        frame = Image.fromarray(np.clip(arr + noise, 0, 255).astype(np.uint8))
        frame.save(outdir / f"frame_{i:05d}.png")

    (outdir / "ground_truth.json").write_text(json.dumps(
        {"fps": FPS, "duration": DURATION,
         "cues": [{"start": s, "end": e, "text": txt} for s, e, txt in CUES]},
        ensure_ascii=False, indent=2))
    print(f"wrote {n_frames} frames to {outdir}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "test_frames")
