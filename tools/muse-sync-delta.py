#!/usr/bin/env python3
# MUSE timebase check: the HD sync edge of every line, found independently in a
# demodulated dump, against the position the reader's timebase gave the line.
#
#   muse-sync-delta.py <fs> <demod dump> <timebase prefix> <out>
#
# <fs> is the demodulated rate (31.25e6 for a 62.5 MHz capture), <demod dump>
# the MUSELD_DUMP_DEMOD file and <timebase prefix> the MUSELD_DUMP_TIMEBASE
# prefix of the same run (the reader's <prefix>.muse.f64 records, one per line:
# line count, MUSE line number, stream position of pixel 1, NaN, input
# samples per pixel, 1).  Writes <out>.csv and <out>.png.
#
# The HD sync occupies pixels 1-12 of every line: a plateau over pixels 1-5, an
# edge between pixels 5 and 7, the opposite plateau over pixels 8-10.  Positive
# on line 1 and the even lines from 4, negative on the others.  Per line the
# plateau levels are the means of the raw samples over pixels 1.5-4.5 and
# 7.5-10, and the edge position is where a cubic spline through the raw samples
# crosses their midpoint.  "delta" is that crossing minus the reader's pixel 1
# position, minus the median of the same over all lines of that polarity, in
# ns: the reader's line-to-line timing error, independent of its own edge
# detector.  The single-edge measurement noise comes from the second
# differences of consecutive lines' crossings (variance 6 R), and the reader's
# residual is what remains in delta beyond that.  (The fourth record field held
# the old DPLL's phase error; it is reported when present.)
import sys
import numpy as np
from scipy.interpolate import CubicSpline
from PIL import Image, ImageDraw

FS = float(sys.argv[1]) if len(sys.argv) > 1 else 31.25e6
DUMP = sys.argv[2]
PREFIX = sys.argv[3]
OUT = sys.argv[4]
PIX = FS / 16.2e6                      # demodulated samples per MUSE pixel
NS = 1e9 / FS                          # ns per demodulated sample

x = np.fromfile(DUMP, dtype=np.float32)
r = np.fromfile(PREFIX + '.muse.f64', dtype=np.float64).reshape(-1, 6)
count, mline, t1, err, ips, state = r.T
# the dump's first sample's stream position (MUSELD_DUMP_DEMOD_AFTER_SEEK writes <dump>.pos)
try:
    with open(DUMP + '.pos') as f:
        t1 = t1 - float(f.read().strip())
except FileNotFoundError:
    pass
print(f"dump {len(x)/FS:.2f} s, {len(r)} line records: "
      f"{int((state==0).sum())} searching, {int((state==2).sum())} locked horizontally, {int((state==1).sum())} locked")
inside = (t1 > 8) & (t1 + 14 * PIX < len(x) - 8)
sel = np.nonzero(inside & (state == 1))[0]
print(f"locked lines inside the dump: {len(sel)}")
if len(sel) == 0:
    sys.exit("nothing to measure: lock came after the dump ended -- raise MUSELD_DUMP_DEMOD_BLOCKS")

positive = (mline == 1) | ((mline > 3) & (mline % 2 == 0))

# --- average raw waveform at the reader's line starts, per polarity (documentation of the shape) ---
W = int(14 * PIX) + 8
for name, pol in (("positive", True), ("negative", False)):
    rows = sel[positive[sel] == pol][:5000]
    acc = np.zeros(W)
    for k in rows:
        i = int(np.floor(t1[k])) - 4
        acc += x[i:i + W]
    acc /= max(len(rows), 1)
    pix = (np.arange(W) - 4) / PIX + 1   # pixel coordinate, approximate (floor alignment)
    print(f"{name} sync, mean of {len(rows)} lines, raw samples labelled by pixel:")
    print("  " + " ".join(f"{p:5.1f}" for p in pix[:W]))
    print("  " + " ".join(f"{v:5.0f}" for v in acc[:W]))

# --- independent edge measurement per locked line ---
t_det = np.full(len(r), np.nan)
lo_lvl = np.full(len(r), np.nan)
hi_lvl = np.full(len(r), np.nan)
grid = np.arange(0, W - 1, 0.02)
for k in sel:
    i0 = int(np.floor(t1[k])) - 4
    w = x[i0:i0 + W].astype(np.float64)
    pix = (np.arange(W) + i0 - t1[k]) / PIX + 1   # exact pixel coordinate of each raw sample
    a = w[(pix >= 1.5) & (pix <= 4.5)].mean()
    b = w[(pix >= 7.5) & (pix <= 10.0)].mean()
    if positive[k] != (b > a):
        continue                                   # no edge of the expected polarity
    mid = 0.5 * (a + b)
    cs = CubicSpline(np.arange(W), w)
    g = grid[((grid + i0 - t1[k]) / PIX + 1 >= 4.5) & ((grid + i0 - t1[k]) / PIX + 1 <= 8.0)]
    y = cs(g) - mid
    s = np.sign(y)
    cross = np.nonzero((s[:-1] <= 0) & (s[1:] > 0))[0] if positive[k] else np.nonzero((s[:-1] >= 0) & (s[1:] < 0))[0]
    if len(cross) == 0:
        continue
    j = cross[0]
    frac = y[j] / (y[j] - y[j + 1])
    t_det[k] = i0 + g[j] + frac * (g[j + 1] - g[j])
    lo_lvl[k] = min(a, b)
    hi_lvl[k] = max(a, b)

found = ~np.isnan(t_det)
print(f"edges found: {found.sum()} of {len(sel)} locked lines ({found.sum()/len(sel)*100:.1f}%)")
off = t_det - t1
ref = {}
for name, pol in (("positive", True), ("negative", False)):
    m = found & (positive == pol)
    ref[pol] = np.median(off[m])
    print(f"{name}: crossing at pixel {1 + ref[pol] / PIX:.3f} of the reader's grid (median), "
          f"plateaus {np.median(lo_lvl[m]):.0f} / {np.median(hi_lvl[m]):.0f}")
delta = np.full(len(r), np.nan)
for pol in (True, False):
    m = found & (positive == pol)
    delta[m] = (off[m] - ref[pol]) * NS

d = delta[found]
print(f"delta (edge - grid): std {d.std():.2f} ns, mean |delta| {np.abs(d).mean():.2f} ns, "
      f"p99 |delta| {np.percentile(np.abs(d), 99):.1f} ns, max |delta| {np.abs(d).max():.1f} ns"
      f" (pixel = {1000/16.2:.1f} ns)")
# single-edge measurement noise from the second differences of consecutive lines' crossings
tt = np.where(found, t_det, np.nan)
d2 = tt[2:] - 2 * tt[1:-1] + tt[:-2]
d2 = d2[~np.isnan(d2)]
mad = np.median(np.abs(d2 - np.median(d2)))
sigma_d2 = 1.4826 * mad
print(f"single-edge measurement noise (from {len(d2)} second differences, robust): "
      f"{sigma_d2 / np.sqrt(6) * NS:.2f} ns rms")
# the loop's own phase error, if the records carry one (the old DPLL did), converted with the edge slope
slope = (np.nanmedian(hi_lvl) - np.nanmedian(lo_lvl)) / 1.4   # level units per pixel, over the ~1.4 pixel edge
e = err[sel]; e = e[~np.isnan(e)]
if len(e):
    print(f"loop phase error: std {e.std():.1f} level units, about {e.std() / slope * 1000 / 16.2:.1f} ns "
          f"at {slope:.0f} units per pixel")
print(f"input samples per pixel {ips[sel].mean():.6f}, "
      f"range {(ips[sel].min()/ips[sel].mean()-1)*1e6:+.0f}..{(ips[sel].max()/ips[sel].mean()-1)*1e6:+.0f} ppm")

# line period from the independent crossings, smoothed over 32 lines: the wow the loop has to follow
per = np.full(len(r), np.nan)
ok = found[:-1] & found[1:] & (np.diff(count) == 1)
per[:-1][ok] = (t_det[1:] - t_det[:-1])[ok]
per_sm = np.full(len(r), np.nan)
nom = 480 * PIX
per_sm[sel[0]:sel[-1] + 1 - 31] = np.convolve(np.nan_to_num(per[sel[0]:sel[-1] + 1], nan=nom), np.ones(32) / 32, 'valid')
print(f"line period, 32-line mean: {(np.nanmin(per_sm)/nom-1)*1e6:+.0f}..{(np.nanmax(per_sm)/nom-1)*1e6:+.0f} ppm "
      f"of nominal, overall {(np.nanmean(per)/nom-1)*1e6:+.0f} ppm")

with open(OUT + '.csv', 'w') as f:
    f.write("count,muse_line,dpll_pixel1,edge,delta_ns,loop_error\n")
    for k in sel:
        f.write(f"{int(count[k])},{int(mline[k])},{t1[k]:.3f},"
                f"{'' if np.isnan(t_det[k]) else f'{t_det[k]:.3f}'},"
                f"{'' if np.isnan(delta[k]) else f'{delta[k]:.2f}'},"
                f"{'' if np.isnan(err[k]) else f'{err[k]:.2f}'}\n")

# --- plot ---
PW, PH = 1400, 900
img = Image.new('RGB', (PW, PH), (255, 255, 255))
dr = ImageDraw.Draw(img)

def panel(y0, h, k0, k1, values, title, yscale, unit, center=0.0):
    x0, w = 60, PW - 90
    dr.rectangle([x0, y0, x0 + w, y0 + h], outline=(0, 0, 0))
    dr.text((x0, y0 - 14), title, fill=(0, 0, 0))
    mid = y0 + h // 2
    dr.line([(x0, mid), (x0 + w, mid)], fill=(200, 200, 200))
    dr.text((x0 + w - 110, y0 + 2), f"{center:g} +-{yscale:g} {unit}", fill=(100, 100, 100))
    span = k1 - k0
    per_px = span / w
    def ypix(v):
        return mid - int(np.clip(v - center, -yscale, yscale) / yscale * (h // 2))
    if per_px > 1.5:
        for px in range(w):
            a = k0 + int(px * per_px); b = k0 + int((px + 1) * per_px)
            seg = values[a:b]
            seg = seg[~np.isnan(seg)]
            if len(seg) == 0:
                continue
            dr.line([(x0 + px, ypix(seg.max())), (x0 + px, ypix(seg.min()))], fill=(30, 60, 200))
    else:
        for k in range(k0, k1):
            v = values[k]
            if np.isnan(v):
                continue
            px = x0 + int((k - k0) / span * w)
            py = ypix(v)
            dr.ellipse([px - 1, py - 1, px + 1, py + 1], fill=(30, 60, 200))
    dr.text((x0, y0 + h + 2), f"line record {k0}", fill=(0, 0, 0))
    dr.text((x0 + w - 110, y0 + h + 2), f"line record {k1}", fill=(0, 0, 0))

k0, k1 = int(sel[0]), int(sel[-1]) + 1
panel(30, 230, k0, k1, delta, "full run: independent sync edge - DPLL grid, per line (envelope per pixel column)", 30, "ns")
worst = int(np.nanargmax(np.abs(delta)))
z0 = max(k0, worst - 300); z1 = min(k1, worst + 300)
panel(320, 230, z0, z1, delta, f"zoom on the largest delta (line record {worst}): one point per line", 30, "ns")
panel(610, 230, k0, k1, (per_sm / nom - 1) * 1e6,
      "line period from the independent edges, 32-line mean, ppm of nominal: the wow the loop follows", 3000, "ppm")
img.save(OUT + '.png')
print("wrote", OUT + ".csv/.png")
