#!/usr/bin/env python3
# Per-line timing error of the player's curve T(k) measured against the color
# burst: the burst phase relative to the sync edge advances exactly 180 deg per
# lattice line, so phi_k - k*pi (mod pi) is constant when the curve is right
# and its deviation is the curve's timing error, 360 deg per subcarrier cycle
# (279 ns).  Content-independent, unlike any threshold on the sync edge.
import sys
import numpy as np
from PIL import Image, ImageDraw

FS = float(sys.argv[4]) if len(sys.argv) > 4 else 31.25e6
FSC = 315e6 / 88
prefix = sys.argv[1] if len(sys.argv) > 1 else '/home/staffanu/wsq-tb'
dump = sys.argv[2] if len(sys.argv) > 2 else '/home/staffanu/wsq-full.f32'
out = sys.argv[3] if len(sys.argv) > 3 else '/home/staffanu/wsq-burst-error'

c = np.fromfile(prefix + '.curve.f64', dtype=np.float64).reshape(-1, 2)
K = c[:, 0].astype(np.int64); T = c[:, 1]
assert np.all(np.diff(K) == 1)

# burst window: 5.6..7.4 us after the sync leading edge
W0 = int(5.6e-6 * FS); W1 = int(7.4e-6 * FS); WN = W1 - W0
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
            ref = np.exp(-2j * np.pi * FSC / FS * n_rel)
            seg = seg - np.nanmean(seg, axis=1, keepdims=True)
            z = np.nansum(seg * ref, axis=1)
            ok = np.isfinite(seg).all(axis=1)
            phase[sel[ok]] = np.angle(z[ok])
            amp[sel[ok]] = np.abs(z[ok]) / WN
        base = hi

# remove the per-line alternation and any pi ambiguity: work on 2*psi
psi2 = np.mod(2 * (phase - K * np.pi), 2 * np.pi)
med_amp = np.nanmedian(amp)
print("burst amplitude percentiles 1/5/10/50:", [round(float(np.nanpercentile(amp, p)), 3) for p in (1, 5, 10, 50)])
# lines without a burst (vertical interval, dropouts) would enter the unwrap
# with a random branch each, so keep only lines with a real burst
valid = np.isfinite(psi2) & (amp > 0.6 * med_amp)
print(f"lines {len(T)}, with a usable burst {valid.sum()} ({valid.mean()*100:.1f}%), "
      f"median burst amplitude {med_amp:.3f}")
# unwrap within each run of consecutive valid lines (a field, typically) and
# remove a linear trend per run (the source's subcarrier is ~2 ppm off
# 227.5 x line rate, a constant phase slope that means nothing for the
# picture): within-field deviations are then unambiguous as long as they are
# smooth; the run-to-run offsets are only known mod 140 ns
err = np.full(len(T), np.nan)
run_offsets = []
vi = np.nonzero(valid)[0]
breaks = np.nonzero(np.diff(vi) > 1)[0] + 1
for seg in np.split(vi, breaks):
    if len(seg) < 20:
        continue
    u = np.unwrap(psi2[seg]) / 2.0 / (2 * np.pi * FSC) * 1e6
    fit = np.polyfit(seg, u, 1)
    err[seg] = u - np.polyval(fit, seg)
    run_offsets.append(np.mod(np.polyval(fit, seg[0]), 1e6 / FSC / 2))
e = err[np.isfinite(err)]
print(f"curve timing error vs burst within fields: p1 {np.percentile(e,1)*1e3:+.0f} ns, p99 {np.percentile(e,99)*1e3:+.0f} ns, "
      f"max |err| {np.max(np.abs(e))*1e3:.0f} ns; |err|>70ns (1 px) {np.mean(np.abs(e)>0.07)*100:.2f}%")
d = np.diff(e)
print(f"line-to-line jitter: std {np.std(d)/np.sqrt(2)*1e3:.1f} ns, p99 |diff| {np.percentile(np.abs(d),99)*1e3:.0f} ns")
ro = np.array(run_offsets)
ro = np.mod(ro - np.median(ro) + 1e6 / FSC / 4, 1e6 / FSC / 2) - 1e6 / FSC / 4
print(f"field-to-field offset (mod 140 ns): {len(ro)} runs, std {np.std(ro)*1e3:.0f} ns, p1/p99 {np.percentile(ro,1)*1e3:+.0f}/{np.percentile(ro,99)*1e3:+.0f} ns")
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
    for band in (0.07, -0.07):
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
panel(30, 230, 0, len(T), "full run: curve timing error vs color burst, per line (envelope per pixel column); red = +-1 pixel", 0.5)
worst = int(np.nanargmax(np.abs(err)))
z0 = max(0, worst - 600); z1 = min(len(T), worst + 1400)
panel(320, 230, z0, z1, f"zoom on the largest error (line {worst}): one point per line", 0.5)
panel(610, 230, 100000, 102000, "a typical region: one point per line", 0.5)
img.save(out + '.png')
print("wrote", out + '.png')
