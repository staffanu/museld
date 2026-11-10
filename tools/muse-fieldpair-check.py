# muse-fieldpair-check.py <file> <u16|m16> <out.npy> [first frame] [frame count]
# Then muse-fieldpair-analyze.py <out.npy>.  Input as for muse-ctlscan.py.
# Frame-complete test: for every boundary between consecutive fields, the mean absolute difference
# of the horizontally blurred luminance (field 0 vertically interpolated onto field 1's lines).
# Boundaries alternate within-frame (F0(k),F1(k)) and cross-frame (F1(k),F0(k+1)).
# Conventional MUSE: the encoder's inter-field prefilter slides one field at a time -> both kinds alike.
# Frame-complete (NHK 1994 improved encoder, "MUSE-III"): fields of one frame share both source
# fields -> within < cross.  A River Runs Through It (PILH-1006) measured symmetric (2026-10-12).
# Caveats: field 0 is the lower field (get this wrong and vertical motion fakes an asymmetry); on a
# 2:3 pulldown film transfer never condition on a neighbouring boundary's motion, a motion boundary
# is always followed by a still one.  The analyzer's "source-identical boundaries" test uses that
# cadence: both kinds must agree unless the encoder pairs fields per frame.
import sys, numpy as np
from scipy.ndimage import gaussian_filter1d
path, kind, out = sys.argv[1], sys.argv[2], sys.argv[3]
first = int(sys.argv[4]) if len(sys.argv) > 4 else 0
W, H = 480, 1125; FR = W*H
dt = np.uint16 if kind == "u16" else np.int16
import os
NF = os.path.getsize(path) // (FR*2)
count = int(sys.argv[5]) if len(sys.argv) > 5 else NF - first
R0, R1 = 10, 500          # field rows used (of the 516-row field grid at 46 / 608)
C0, C1 = 106, 480         # luminance columns
SIG = 4.0
CH = 60
rec = []                  # per boundary: frame k, kind (0 within, 1 cross), d, motion (frame diff F1(k) vs F1(k+1))
prev_f1 = None; prev_f0b = None
def fields(fr):
    f0 = fr[46+R0-1:46+R1, C0:C1].astype(np.float32)   # one extra row above: field 0 is the lower field, f1[i] lies between f0[i-1] and f0[i]
    f1 = fr[608+R0:608+R1, C0:C1].astype(np.float32)
    f0 = gaussian_filter1d(f0, SIG, axis=1); f1 = gaussian_filter1d(f1, SIG, axis=1)
    f0i = 0.5*(f0[:-1] + f0[1:])                         # field 0 at field 1's line positions
    return f0i, f1
n = first
while n < first + count:
    m = min(CH, first + count - n)
    buf = np.fromfile(path, dt, count=m*FR, offset=n*FR*2).reshape(m, H, W)
    for i in range(m):
        f0i, f1 = fields(buf[i])
        if prev_f1 is not None:
            rec.append((n+i-1, 1, np.abs(prev_f1 - f0i).mean(), np.abs(prev_f1 - f1).mean()))
        rec.append((n+i, 0, np.abs(f0i - f1).mean(), 0.0))
        prev_f1 = f1
    n += m
    if (n - first) % 3000 < CH: print(n, "frames", flush=True)
rec = np.array(rec, np.float64); np.save(out, rec)
w = rec[rec[:,1]==0, 2]; c = rec[rec[:,1]==1, 2]
print(f"frames {first}..{n}: within-frame boundaries mean d = {w.mean():.4f} (median {np.median(w):.4f}), "
      f"cross-frame mean d = {c.mean():.4f} (median {np.median(c):.4f})")
k = min(len(w), len(c)) - 1
r = (w[1:k+1] - c[:k]) / (w[1:k+1] + c[:k] + 1e-6)      # pair each cross boundary with the following within boundary
print(f"fraction of frame pairs with within < cross: {(r < 0).mean():.3f}, median asymmetry {np.median(r):+.4f}")
