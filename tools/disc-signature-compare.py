#!/usr/bin/env python3
"""Compare two disc signatures recorded with museld --signature-write.

  disc-signature-compare.py A.ldsig B.ldsig [--window FRAMES] [--grid COLSxROWS] [--fields]

The frames of the two recordings are paired by their disc frame number and cut
into windows of consecutive frames.  Each window gets two scores, both a
correlation over all its block values:

  spatial   each frame's block pattern, the frame's mean removed
  temporal  each block's change within the window, the block's mean removed

"same place" are the windows as paired; "elsewhere" pairs each window of A
with a window of B at least three seconds away, which is what two recordings
that are not the same picture look like.  Two plays of one disc should have
their lowest same-place score above the highest elsewhere score.
"""
import argparse
import numpy as np


def load(path):
    h = open(path, "rb").read(64)
    if h[:6] != b"LDSIG\0":
        raise SystemExit(f"{path}: not a disc signature")
    fields, rows, cols = h[9], h[10], h[11]
    record_size, fps_num, fps_den = np.frombuffer(h[12:24], "<u4")
    offset, scale = np.frombuffer(h[24:32], "<f4")
    record = np.dtype([("frame", "<i4"), ("chapter", "<i2"), ("flags", "u1"), ("zero", "u1"),
                       ("blocks", "<u2", (fields, rows, cols))])
    if record.itemsize != record_size:
        raise SystemExit(f"{path}: unexpected record size {record_size}")
    z = np.fromfile(path, dtype=record, offset=64)
    return {"standard": ("NTSC", "PAL", "MUSE")[h[8]], "fps": fps_num / fps_den, "frame": z["frame"],
            "flags": z["flags"], "blocks": z["blocks"] / scale + offset}


def features(sig, grid, per_field):
    """n frames x (1 or 2 fields) x blocks, keeping the frames whose code continues the previous one's"""
    b = sig["blocks"]
    n, fields, rows, cols = b.shape
    gx, gy = grid
    b = b.reshape(n, fields, gy, rows // gy, gx, cols // gx).mean(axis=(3, 5)).reshape(n, fields, gy * gx)
    if not per_field:
        b = b.mean(axis=1, keepdims=True)
    frame = sig["frame"]
    good = (frame >= 0) & ((sig["flags"] & 16) != 0)
    good[1:] &= np.diff(frame) == 1
    return frame[good], b[good]


def corr(a, b):
    a = a - a.mean()
    b = b - b.mean()
    d = np.sqrt((a * a).sum() * (b * b).sum())
    return float((a * b).sum() / d) if d > 0 else 0.0


def scores(p, r):
    p = p.reshape(-1, p.shape[-1])  # rows: frames (or fields), columns: blocks
    r = r.reshape(-1, r.shape[-1])
    spatial = corr(p - p.mean(axis=1, keepdims=True), r - r.mean(axis=1, keepdims=True))
    temporal = corr(p - p.mean(axis=0), r - r.mean(axis=0))
    return spatial, temporal


def describe(name, values):
    if len(values) == 0:
        print(f"  {name:10s} no windows")
        return
    v = np.array(values)
    for i, method in enumerate(("spatial", "temporal")):
        print(f"  {name:10s} {method:8s} n={len(v):5d}  min {v[:, i].min():+.3f}  1% {np.percentile(v[:, i], 1):+.3f}  "
              f"median {np.median(v[:, i]):+.3f}  99% {np.percentile(v[:, i], 99):+.3f}  max {v[:, i].max():+.3f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--window", type=int, default=60, help="frames per window (default 60)")
    ap.add_argument("--grid", default="8x6", help="columns x rows the 16x12 blocks are reduced to (default 8x6)")
    ap.add_argument("--fields", action="store_true", help="score the two fields separately instead of their mean")
    args = ap.parse_args()
    grid = tuple(int(x) for x in args.grid.split("x"))
    if 16 % grid[0] or 12 % grid[1]:
        raise SystemExit("--grid must divide 16x12")

    sa, sb = load(args.a), load(args.b)
    for path, s in ((args.a, sa), (args.b, sb)):
        f = s["frame"]
        coded = f[f >= 0]
        span = f"disc frames {coded.min()}..{coded.max()}" if len(coded) else "no disc code"
        print(f"{path}: {s['standard']}, {len(f)} frames, {len(coded)} with a code, {span}")
    if sa["standard"] != sb["standard"]:
        raise SystemExit("the recordings are of different standards")

    ka, fa = features(sa, grid, args.fields)
    kb, fb = features(sb, grid, args.fields)
    index_b = {}
    for i, k in enumerate(kb):
        index_b.setdefault(int(k), i)
    pairs = [(i, index_b[int(k)]) for i, k in enumerate(ka) if int(k) in index_b]
    print(f"{len(pairs)} disc frames in both")
    L = args.window
    rng = np.random.default_rng(1)
    same, elsewhere = [], []
    for s in range(0, len(pairs) - L + 1, max(1, L // 2)):
        ia, ib = pairs[s]
        ja, jb = pairs[s + L - 1]
        if ja - ia != L - 1 or jb - ib != L - 1:
            continue  # a gap or a repeat in one of the recordings
        p = fa[ia:ia + L]
        same.append(scores(p, fb[ib:ib + L]))
        for _ in range(4):
            o = int(rng.integers(0, max(1, len(fb) - L)))
            if abs(o - ib) > 3 * sa["fps"] and o + L <= len(fb) and kb[o + L - 1] - kb[o] == L - 1:
                elsewhere.append(scores(p, fb[o:o + L]))
    print(f"windows of {L} frames, {grid[0]}x{grid[1]} blocks{', per field' if args.fields else ''}:")
    describe("same place", same)
    describe("elsewhere", elsewhere)


main()
