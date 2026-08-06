#!/usr/bin/env python3
"""OCR burned-in Japanese subtitles from a directory of frames into an .srt.

Prototype for museld's live subtitle OCR. Pipeline per frame (bottom band only):
cheap text-presence mask -> skip OCR when the mask didn't change -> RapidOCR
(PP-OCR models over ONNX Runtime, same engine family we'd embed in museld) ->
fuzzy grouping of consecutive detections into cues -> SRT.

Usage: subocr.py FRAMES_DIR [--fps N] [--out ja.srt] [--score]
--score compares against FRAMES_DIR/ground_truth.json and prints accuracy.
"""

import argparse
import difflib
import json
import sys
import time
from collections import Counter
from pathlib import Path

import numpy as np
from PIL import Image

BAND_FRAC = 0.28          # bottom fraction of the frame that may hold subtitles
MASK_DIFF_THRESH = 0.15   # relative mask change that triggers a fresh OCR
SIM_THRESH = 0.55         # difflib ratio above which two readings are one cue
MIN_CUE_FRAMES = 2        # ignore cues shorter than this many sampled frames


def text_mask(band):
    """Binary mask of subtitle-ish pixels: bright, low-saturation, dark edge nearby."""
    arr = np.asarray(band, dtype=np.int16)
    lum = arr.mean(axis=2)
    sat = arr.max(axis=2) - arr.min(axis=2)
    bright = (lum > 190) & (sat < 40)
    dark = lum < 70
    # dark outline within a 5px neighborhood (cheap dilation via shifts)
    near_dark = np.zeros_like(dark)
    for dy in (-5, 0, 5):
        for dx in (-5, 0, 5):
            near_dark |= np.roll(np.roll(dark, dy, axis=0), dx, axis=1)
    return bright & near_dark


def mask_changed(prev, cur):
    if prev is None:
        return True
    a, b = prev.sum(), cur.sum()
    if max(a, b) < 200:            # both essentially empty
        return (a > 200) != (b > 200)
    return (prev ^ cur).sum() / max(a, b) > MASK_DIFF_THRESH


def norm(s):
    return "".join(s.split())


def similar(a, b):
    return difflib.SequenceMatcher(None, norm(a), norm(b)).ratio()


def fmt_ts(t):
    ms = int(round(t * 1000))
    return f"{ms // 3600000:02d}:{ms // 60000 % 60:02d}:{ms // 1000 % 60:02d},{ms % 1000:03d}"


def write_srt(cues, path):
    with open(path, "w", encoding="utf-8") as f:
        for i, c in enumerate(cues, 1):
            f.write(f"{i}\n{fmt_ts(c['start'])} --> {fmt_ts(c['end'])}\n{c['text']}\n\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("frames_dir", type=Path)
    ap.add_argument("--fps", type=float, default=None)
    ap.add_argument("--out", type=Path, default=None)
    ap.add_argument("--score", action="store_true")
    args = ap.parse_args()

    gt_path = args.frames_dir / "ground_truth.json"
    gt = json.loads(gt_path.read_text()) if gt_path.exists() else None
    fps = args.fps or (gt and gt["fps"]) or 3.0
    out = args.out or args.frames_dir / "ja.srt"

    from rapidocr import RapidOCR
    ocr = RapidOCR()

    frames = sorted(args.frames_dir.glob("frame_*.png"))
    if not frames:
        sys.exit(f"no frames in {args.frames_dir}")

    # raw per-frame readings: list of (frame_index, text or "")
    readings = []
    prev_mask, last_text = None, ""
    n_ocr, ocr_time = 0, 0.0
    for i, fp in enumerate(frames):
        img = Image.open(fp).convert("RGB")
        band = img.crop((0, int(img.height * (1 - BAND_FRAC)), img.width, img.height))
        m = text_mask(band)
        if mask_changed(prev_mask, m):
            t0 = time.perf_counter()
            if m.sum() < 200:
                last_text = ""
            else:
                res = ocr(np.asarray(band))
                lines = [t for t in (res.txts or [])] if res is not None else []
                last_text = "\n".join(lines)
            ocr_time += time.perf_counter() - t0
            n_ocr += 1
        prev_mask = m
        readings.append((i, last_text))

    # group consecutive similar readings into cues
    cues = []
    cur = None
    for i, text in readings:
        if not text:
            if cur:
                cues.append(cur)
                cur = None
            continue
        if cur and similar(text, cur["variants"][-1]) >= SIM_THRESH:
            cur["variants"].append(text)
            cur["end_i"] = i
        else:
            if cur:
                cues.append(cur)
            cur = {"variants": [text], "start_i": i, "end_i": i}
    if cur:
        cues.append(cur)

    result = []
    for c in cues:
        if len(c["variants"]) < MIN_CUE_FRAMES:
            continue
        text = Counter(c["variants"]).most_common(1)[0][0]
        result.append({"start": c["start_i"] / fps,
                       "end": (c["end_i"] + 1) / fps,
                       "text": text})

    write_srt(result, out)
    print(f"{len(frames)} frames, {n_ocr} OCR calls ({ocr_time / max(n_ocr, 1) * 1000:.0f} ms avg), "
          f"{len(result)} cues -> {out}")

    if args.score and gt:
        matched = 0
        char_ratios = []
        for g in gt["cues"]:
            best, best_sim = None, 0.0
            for r in result:
                if r["end"] > g["start"] and r["start"] < g["end"]:
                    s = similar(r["text"], g["text"])
                    if s > best_sim:
                        best, best_sim = r, s
            if best:
                matched += 1
                char_ratios.append(best_sim)
                dt_start = abs(best["start"] - g["start"])
                dt_end = abs(best["end"] - g["end"])
                flag = "" if best_sim > 0.95 else f"   OCR: {best['text']!r}"
                print(f"  [{g['start']:6.1f}s] sim={best_sim:.2f} "
                      f"Δstart={dt_start:.2f}s Δend={dt_end:.2f}s  {g['text']!r}{flag}")
            else:
                print(f"  [{g['start']:6.1f}s] MISSED  {g['text']!r}")
        extra = len(result) - matched
        print(f"cues: {matched}/{len(gt['cues'])} detected, {extra} spurious, "
              f"mean text similarity {np.mean(char_ratios):.3f}" if char_ratios else "no matches")


if __name__ == "__main__":
    main()
