#!/usr/bin/env python3
# Per-line timing error of the player's curve T(k) measured against the PAL
# laserdisc pilot burst (IEC 60856 9.1.2): 240 x fH = 3.75 MHz on the sync tip
# of every line, 0.5-4.1 us after the sync edge.  Being an integer number of
# cycles per line, the pilot has the same phase at every line start when the
# curve is right, and its deviation is the curve's timing error, 360 deg per
# pilot cycle (267 ns).  Content-independent, present in the vertical interval
# too, and 60 IRE peak to peak -- the PAL counterpart of ntsc-burst-check.py.
#
#   pal-pilot-check.py <timebase prefix> <demod dump> <out> [<demod rate>]
#
# with the dumps from one run: MUSELD_DUMP_TIMEBASE=<prefix> and
# MUSELD_DUMP_DEMOD=<dump> (float32 at the video-decimated rate, 20 MHz for a
# 40 MHz capture).  <prefix>.curve.f64 is the hsync curve before the reader's
# own pilot refinement; <prefix>.pilot.f64 the refined line starts, in the
# same format -- copy it to <other>.curve.f64 to measure the residual.
import sys
import numpy as np
from PIL import Image, ImageDraw

FS = float(sys.argv[4]) if len(sys.argv) > 4 else 20e6
FP = 3.75e6
prefix = sys.argv[1]
dump = sys.argv[2]
out = sys.argv[3]

c = np.fromfile(prefix + '.curve.f64', dtype=np.float64).reshape(-1, 2)
K = c[:, 0].astype(np.int64); T = c[:, 1]
assert np.all(np.diff(K) == 1)

# pilot window: 0.8..3.8 us after the sync leading edge, inside the 0.5-4.1 us
# the burst occupies, clear of the filtered edges
W0 = int(0.8e-6 * FS); W1 = int(3.8e-6 * FS); WN = W1 - W0
phase = np.full(len(T), np.nan)
amp = np.full(len(T), np.nan)
CH = 64 * 1024 * 1024
with open(dump, 'rb') as f:
    base = 0
    while True:
        x = np.fromfile(f, dtype=np.float32, count=CH)
        if x.size == 0:
            break
        lo = base; hi = base + x.size
        sel = np.nonzero((T + W0 >= lo) & (T + W1 + 1 < hi))[0]
        if len(sel):
            start = np.floor(T[sel]).astype(np.int64) + W0 - lo
            frac = T[sel] - np.floor(T[sel])
            idx = start[:, None] + np.arange(WN)[None, :]
            seg = x[idx].astype(np.float64)
            n_rel = (np.arange(WN)[None, :] + W0 - frac[:, None])   # samples after T(k)
            ref = np.exp(-2j * np.pi * FP / FS * n_rel)
            seg = seg - np.nanmean(seg, axis=1, keepdims=True)
            z = np.nansum(seg * ref, axis=1)
            ok = np.isfinite(seg).all(axis=1)
            phase[sel[ok]] = np.angle(z[ok])
            amp[sel[ok]] = np.abs(z[ok]) / WN
        base = hi

med_amp = np.nanmedian(amp)
print("pilot amplitude percentiles 1/5/10/50:", [round(float(np.nanpercentile(amp, p)), 3) for p in (1, 5, 10, 50)])
# lines without a pilot (a dropout through the tip, the optional half-line
# bursts missing in the vertical interval) would enter the unwrap with a
# random branch each, so keep only lines with a real one
valid = np.isfinite(phase) & (amp > 0.6 * med_amp)
print(f"lines {len(T)}, with a usable pilot {valid.sum()} ({valid.mean()*100:.1f}%), "
      f"median pilot amplitude {med_amp:.3f}")
# unwrap within each run of consecutive valid lines and remove a linear trend
# per run (a constant phase slope is the pilot's frequency a few ppm off
# 240 x line rate, not a timing error); the run-to-run offsets are only
# known mod 267 ns
err = np.full(len(T), np.nan)
run_offsets = []
vi = np.nonzero(valid)[0]
breaks = np.nonzero(np.diff(vi) > 1)[0] + 1
for seg in np.split(vi, breaks):
    if len(seg) < 20:
        continue
    u = np.unwrap(phase[seg]) / (2 * np.pi * FP) * 1e6
    fit = np.polyfit(seg, u, 1)
    err[seg] = u - np.polyval(fit, seg)
    run_offsets.append(np.mod(np.polyval(fit, seg[0]), 1e6 / FP))
e = err[np.isfinite(err)]
print(f"curve timing error vs pilot within runs: p1 {np.percentile(e,1)*1e3:+.0f} ns, p99 {np.percentile(e,99)*1e3:+.0f} ns, "
      f"max |err| {np.max(np.abs(e))*1e3:.0f} ns; |err|>56ns (1 px) {np.mean(np.abs(e)>0.056)*100:.2f}%")
d = np.diff(e)
print(f"line-to-line jitter: std {np.std(d)/np.sqrt(2)*1e3:.1f} ns, p99 |diff| {np.percentile(np.abs(d),99)*1e3:.0f} ns")
ro = np.array(run_offsets)
ro = np.mod(ro - np.median(ro) + 1e6 / FP / 2, 1e6 / FP) - 1e6 / FP / 2
print(f"run-to-run offset (mod 267 ns): {len(ro)} runs, std {np.std(ro)*1e3:.0f} ns, p1/p99 {np.percentile(ro,1)*1e3:+.0f}/{np.percentile(ro,99)*1e3:+.0f} ns")
np.save(out + '.npy', err)

W, H = 1400, 900
img = Image.new('RGB', (W, H), (255, 255, 255))
dr = ImageDraw.Draw(img)
def panel(y0, h, k0, k1, title, yscale):
    x0, w = 60, W - 90
    dr.rectangle([x0, y0, x0 + w, y0 + h], outline=(0, 0, 0))
    dr.text((x0, y0 - 14), title, fill=(0, 0, 0))
    mid = y0 + h // 2
    dr.line([(x0, mid), (x0 + w, mid)], fill=(200, 200, 200))
    for band in (0.056, -0.056):
        yy = mid - int(band / yscale * (h // 2))
        dr.line([(x0, yy), (x0 + w, yy)], fill=(230, 200, 200))
    dr.text((x0 + w - 70, y0 + 2), f"+-{yscale*1e3:.0f} ns", fill=(100, 100, 100))
    span = k1 - k0; per_px = span / w
    if per_px > 1.5:
        for px in range(w):
            a = k0 + int(px * per_px); b = k0 + int((px + 1) * per_px)
            seg = err[a:b]; seg = seg[np.isfinite(seg)]
            if len(seg) == 0: continue
            lo = mid - int(np.clip(seg.max(), -yscale, yscale) / yscale * (h // 2))
            hi = mid - int(np.clip(seg.min(), -yscale, yscale) / yscale * (h // 2))
            dr.line([(x0 + px, lo), (x0 + px, hi)], fill=(30, 60, 200))
    else:
        for k in range(k0, k1):
            v = err[k]
            if not np.isfinite(v): continue
            px = x0 + int((k - k0) / span * w)
            py = mid - int(np.clip(v, -yscale, yscale) / yscale * (h // 2))
            dr.ellipse([px - 1, py - 1, px + 1, py + 1], fill=(30, 60, 200))
    dr.text((x0, y0 + h + 2), f"line {k0}", fill=(0, 0, 0))
    dr.text((x0 + w - 90, y0 + h + 2), f"line {k1}", fill=(0, 0, 0))
panel(30, 230, 0, len(T), "full run: curve timing error vs pilot burst, per line (envelope per pixel column); red = +-1 pixel", 0.5)
worst = int(np.nanargmax(np.abs(err)))
z0 = max(0, worst - 600); z1 = min(len(T), worst + 1400)
panel(320, 230, z0, z1, f"zoom on the largest error (line {worst}): one point per line", 0.5)
mid_k = len(T) // 2
panel(610, 230, mid_k, min(len(T), mid_k + 2000), "a typical region: one point per line", 0.5)
img.save(out + '.png')
print("wrote", out + '.png')
