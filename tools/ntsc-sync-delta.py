#!/usr/bin/env python3
# One data point per line: detected sync position minus the reconstructed
# (curve) sync position, in microseconds.  Lines with no hsync pulse
# (vertical interval) are NaN.  Written as CSV and plotted.
import sys
import numpy as np
from scipy import signal
from PIL import Image, ImageDraw

FS = float(sys.argv[1]) if len(sys.argv) > 1 else 31.25e6
DUMP = sys.argv[2] if len(sys.argv) > 2 else '/home/staffanu/wsq-full.f32'
PREFIX = sys.argv[3] if len(sys.argv) > 3 else '/home/staffanu/wsq-tb'
OUT = sys.argv[4] if len(sys.argv) > 4 else '/home/staffanu/wsq-sync-delta'
P = FS / (30000.0 / 1001.0 * 525.0)
DECIM = 8

# --- independent pulse detection over the full demod dump ---
lp = signal.firwin(49, 0.4e6, fs=FS).astype(np.float32)
delay = (len(lp) - 1) / 2.0
zi = np.zeros(len(lp) - 1, dtype=np.float32)
xs = []
with open(DUMP, 'rb') as f:
    while True:
        ch = np.fromfile(f, dtype=np.float32, count=8 * 1024 * 1024)
        if ch.size == 0:
            break
        y, zi = signal.lfilter(lp, 1.0, ch, zi=zi)
        xs.append(y[::DECIM].astype(np.float32))
xf = np.concatenate(xs); del xs
th = 0.15
below = xf < th
fi = np.nonzero(~below[:-1] & below[1:])[0]
ri = np.nonzero(below[:-1] & ~below[1:])[0]
if len(ri) and ri[0] < fi[0]:
    ri = ri[1:]
n = min(len(fi), len(ri)); fi, ri = fi[:n], ri[:n]
w = (ri - fi) * DECIM / FS * 1e6
frc = (xf[fi] - th) / np.maximum(xf[fi] - xf[fi + 1], 1e-6)
t_all = (fi + frc) * DECIM - delay
t_h = t_all[(w > 3.0) & (w < 6.5)]

# --- player curve, one point per line ---
c = np.fromfile(PREFIX + '.curve.f64', dtype=np.float64).reshape(-1, 2)
K = c[:, 0].astype(np.int64); T = c[:, 1]
assert np.all(np.diff(K) == 1), "expected one continuous curve segment"
covered = T < len(xf) * DECIM - 2 * P
K = K[covered]; T = T[covered]
print(f"dump covers {len(xf)*DECIM/FS:.2f} s, {len(T)} curve lines")

# --- assign each pulse to the nearest curve line by time ---
idx = np.searchsorted(T, t_h)
idx = np.clip(idx, 1, len(T) - 1)
use_prev = np.abs(t_h - T[idx - 1]) < np.abs(t_h - T[idx])
line = np.where(use_prev, idx - 1, idx)
delta = (t_h - T[line]) / FS * 1e6           # us, detected - reconstructed
delta_line = np.full(len(T), np.nan)
# keep the closest pulse if two map to one line
order = np.argsort(np.abs(delta))[::-1]
delta_line[line[order]] = delta[order]

valid = ~np.isnan(delta_line)
print(f"lines {len(T)}, with a detected hsync {valid.sum()} ({valid.mean()*100:.1f}%)")
d = delta_line[valid]
print(f"delta: median {np.median(d):+.3f} us, |delta|>2us {np.mean(np.abs(d)>2)*100:.1f}%, "
      f"|delta|>10us {np.mean(np.abs(d)>10)*100:.1f}%")

# walk events: runs of consecutive valid lines with |delta| > 2 us
bad = np.abs(np.nan_to_num(delta_line, nan=0.0)) > 2
edges = np.diff(bad.astype(np.int8))
starts = np.nonzero(edges == 1)[0] + 1
ends = np.nonzero(edges == -1)[0] + 1
if bad[0]: starts = np.concatenate([[0], starts])
if bad[-1]: ends = np.concatenate([ends, [len(bad)]])
runs = ends - starts
if len(runs):
    print(f"walk events (runs with |delta|>2us): {len(runs)}, median length {np.median(runs):.0f} lines, "
          f"longest {runs.max()} lines, lines in runs {runs.sum()} ({runs.sum()/len(T)*100:.1f}%)")
    worst = starts[np.argmax(runs)]
else:
    print("walk events (runs with |delta|>2us): 0")
    # no walk: zoom on the largest single delta instead
    worst = int(np.nanargmax(np.abs(delta_line)))
    print(f"largest single |delta|: {delta_line[worst]:+.3f} us at line {worst}")

# --- CSV ---
with open(OUT + '.csv', 'w') as f:
    f.write("line,curve_pos_samples,delta_us\n")
    for k in range(len(T)):
        f.write(f"{K[k]},{T[k]:.2f},{'' if np.isnan(delta_line[k]) else f'{delta_line[k]:.3f}'}\n")

# --- plot ---
W, H = 1400, 900
img = Image.new('RGB', (W, H), (255, 255, 255))
dr = ImageDraw.Draw(img)

def panel(y0, h, k0, k1, title, yscale):
    x0, w = 60, W - 90
    dr.rectangle([x0, y0, x0 + w, y0 + h], outline=(0, 0, 0))
    dr.text((x0, y0 - 14), title, fill=(0, 0, 0))
    mid = y0 + h // 2
    dr.line([(x0, mid), (x0 + w, mid)], fill=(200, 200, 200))
    for band in (2, -2):
        yy = mid - int(band / yscale * (h // 2))
        dr.line([(x0, yy), (x0 + w, yy)], fill=(230, 200, 200))
    dr.text((x0 + w - 60, y0 + 2), f"+-{yscale:.0f} us", fill=(100, 100, 100))
    span = k1 - k0
    per_px = span / w
    if per_px > 1.5:
        # envelope per pixel column
        for px in range(w):
            a = k0 + int(px * per_px); b = k0 + int((px + 1) * per_px)
            seg = delta_line[a:b]
            seg = seg[~np.isnan(seg)]
            if len(seg) == 0:
                continue
            lo = mid - int(np.clip(seg.max(), -yscale, yscale) / yscale * (h // 2))
            hi = mid - int(np.clip(seg.min(), -yscale, yscale) / yscale * (h // 2))
            dr.line([(x0 + px, lo), (x0 + px, hi)], fill=(30, 60, 200))
    else:
        for k in range(k0, k1):
            v = delta_line[k]
            if np.isnan(v):
                continue
            px = x0 + int((k - k0) / span * w)
            py = mid - int(np.clip(v, -yscale, yscale) / yscale * (h // 2))
            dr.ellipse([px - 1, py - 1, px + 1, py + 1], fill=(30, 60, 200))
    dr.text((x0, y0 + h + 2), f"line {k0}", fill=(0, 0, 0))
    dr.text((x0 + w - 90, y0 + h + 2), f"line {k1}", fill=(0, 0, 0))

panel(30, 230, 0, len(T),
      "full run: detected sync - reconstructed sync, per line (envelope per pixel column)", 35)
z0 = max(0, worst - 600); z1 = min(len(T), worst + 1400)
zoom_scale = 35 if len(runs) else 1
panel(320, 230, z0, z1,
      f"zoom on the {'longest walk event (starts' if len(runs) else 'largest single delta ('}line {worst}): "
      f"one point per line", zoom_scale)
q0 = min(100000, len(T) - 2000); q1 = q0 + 2000
panel(610, 230, q0, q1, "a typical region: one point per line", zoom_scale)
img.save(OUT + '.png')
print("wrote", OUT + ".csv/.png")
