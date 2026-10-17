# muse-fieldpair-analyze.py <out.npy>: summarize a muse-fieldpair-check.py run, overall, by motion
# level and per 5 minutes, plus the pulldown source-identical-boundary comparison.
import sys, numpy as np
rec = np.load(sys.argv[1])
w = rec[rec[:,1]==0]; c = rec[rec[:,1]==1]
k = min(len(w), len(c)) - 1
fr = w[1:k+1,0]; dw = w[1:k+1,2]; dc = c[:k,2]
r = (dw - dc) / (dw + dc + 1e-6)
print(f"{k} frame pairs. overall: within mean {dw.mean():.3f} cross mean {dc.mean():.3f}, frac within<cross {(r<0).mean():.3f}, median asym {np.median(r):+.4f}")
for lo, hi in ((0,1),(1,2),(2,4),(4,8),(8,1e9)):
    m = (dc >= lo) & (dc < hi)
    if m.sum(): print(f"  cross d in [{lo},{hi}): n={m.sum():6d} frac within<cross {(r[m]<0).mean():.3f} median asym {np.median(r[m]):+.4f}")
print("per 5-minute window: frac within<cross, median asym")
for s in range(0, int(fr.max()), 9000):
    m = (fr >= s) & (fr < s+9000)
    if m.sum(): print(f"  {s/30/60:5.1f} min: {(r[m]<0).mean():.3f} {np.median(r[m]):+.4f}")
nb = np.maximum(rec[:-2,2], rec[2:,2]); mid = rec[1:-1,2]; kd = rec[1:-1,1]
sel = (mid < 0.4*nb) & (nb > 6)
ws, cs = mid[sel & (kd==0)], mid[sel & (kd==1)]
if len(ws) > 100 and len(cs) > 100:
    print(f"2:3 pulldown source-identical boundaries: within n={len(ws)} mean {ws.mean():.3f}, cross n={len(cs)} mean {cs.mean():.3f}"
          " (frame-complete encoding would make within clearly smaller)")
else:
    print("no pulldown cadence found (video source or no motion), source-identical test skipped")
