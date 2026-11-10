# muse-ctlscan.py <file> <u16|m16> <out.npy>: the control signal bits of every field of a 16.2 MHz
# MUSE baseband file (u16 capture, or an m16 dump from museld --write-muse16: int16, 4x the 0..255
# baseband, whole 1125x480 frames).  Prints the motion vector and motion information statistics;
# the npy holds per field (frame, field, hv, vv, motion info, sensitivity, inhibit, field phase,
# frame phase).  Frame alignment as PhaseCorrect16MHzFrameReader::compute_initial_skip, re-checked
# every 500 frames.  Used 2026-10 to look for a MUSE-III (NHK 1994 improved encoder) disc: no
# Hi-Vision LD scanned so far sends a nonzero vector or the sensitivity/inhibit bits.
import sys, numpy as np
path, kind, out = sys.argv[1], sys.argv[2], sys.argv[3]
W, H = 480, 1125; FR = W*H
raw = np.memmap(path, np.uint16 if kind == "u16" else np.int16, "r")
NF = raw.shape[0] // FR
H_PAR = np.array([[1,1,1,0,1,0,0,0],[0,1,1,1,0,1,0,0],[1,1,0,1,0,0,1,0],[1,0,1,1,0,0,0,1]])
def find_skip(at):
    b = raw[at:at+2*FR].astype(np.float32)
    n = 2*H - 1
    idx = np.arange(n)[:, None]*W + np.arange(W)[None, :]
    v3, v5, v7 = b[idx+3], b[idx+5], b[idx+7]
    isPos = (v3 < v5) & (v5 < v7); isNeg = (v3 > v5) & (v5 > v7)
    best_p, best_s = 0, -1
    lines = np.arange(2000)
    for p in range(W):
        score = 0
        for s in range(0, 2000, 50):
            ok = False
            for ph in (0, 1):
                exp = ((lines[s:s+50] + ph) % 2 == 0)
                m = np.where(exp, isPos[s:s+50, p], isNeg[s:s+50, p])
                if m.sum() > 47: ok = True
            score += ok
        if score > best_s: best_s, best_p = score, p
    line = np.arange(H); expPos = (line == 0) | ((line > 2) & (line % 2 == 1))
    ip, ineg = isPos[:, best_p].astype(np.float32), isNeg[:, best_p].astype(np.float32)
    sc = np.correlate(ip, expPos.astype(np.float32), "valid") + np.correlate(ineg, (~expPos).astype(np.float32), "valid")
    L = int(np.argmax(sc))
    return L*W + best_p, sc[L]
def control_bits(rows):
    rows = rows.astype(np.float32); thr = (rows.max() + rows.min()) / 2
    groups = []
    for r in range(5):
        for c in range(7, 87, 16):
            bits = np.array([(rows[r, c+2*b] + rows[r, c+2*b+1]) > 2*thr for b in range(8)], int)
            syn = (H_PAR @ bits) % 2; ok = True
            if syn.any():
                m = [i for i in range(8) if (H_PAR[:, i] == syn).all()]
                if m: bits[m[0]] ^= 1
                else: ok = False
            groups.append((tuple(bits[:4]), ok))
    res = []
    for g in range(8):
        votes = {}
        for i in range(3):
            b, ok = groups[g + 8*i]
            if ok: votes[b] = votes.get(b, 0) + 1
        best = max(votes.items(), key=lambda kv: kv[1]) if votes else None
        if best is None or (len(votes) > 1 and best[1] < 2): res += [-1]*4
        else: res += list(best[0])
    return res
def twos(bits):
    if any(b < 0 for b in bits): return -99
    v = sum(b << i for i, b in enumerate(bits))
    return v - (1 << len(bits)) if bits[-1] else v
skip, score = find_skip(0); print("initial skip", skip, "score", score, flush=True)
recs = []; bad = 0; n = 0
while True:
    base = skip + n*FR
    if base + FR > raw.shape[0]: break
    if n % 500 == 0 and n > 0:
        if bad > 50:
            skip2, sc2 = find_skip(base); print(f"frame {n}: {bad} bad fields in last 500, realign {skip}->{skip2} (score {sc2})", flush=True)
            skip = skip2 - n*FR; base = skip + n*FR
        bad = 0
    fr = raw[base:base+FR].reshape(H, W)
    for fld, row0 in ((1, 558), (0, 1120)):   # control at 558.. describes field 1 of this frame, 1120.. the next frame's field 0
        bits = control_bits(fr[row0:row0+5, 12:108])
        if -1 in bits[:8] + bits[15:18]: bad += 1
        hv, vv = twos(bits[1:5]), twos(bits[5:8])
        mi = -1 if -1 in bits[15:18] else bits[15] + 2*bits[16] + 4*bits[17]
        recs.append((n, fld, hv, vv, mi, bits[13], bits[14], bits[0], bits[8]))
    n += 1
    if n % 5000 == 0: print(f"{n} frames", flush=True)
recs = np.array(recs, np.int32); np.save(out, recs)
ok = recs[:, 2] > -99
print(f"{n} frames, {ok.sum()} fields with a valid vector of {len(recs)}")
nz = ok & ((recs[:, 2] != 0) | (recs[:, 3] != 0))
print("fields with nonzero vector:", nz.sum())
if nz.any():
    for r in recs[nz][:40]: print(f"  t={r[0]/30:8.2f}s frame {r[0]} field {r[1]} vector ({r[2]},{r[3]}) motion info {r[4]}")
vals, cnt = np.unique(recs[ok][:, 4], return_counts=True); print("motion info histogram:", dict(zip(vals.tolist(), cnt.tolist())))
vals, cnt = np.unique(recs[ok][:, 5], return_counts=True); print("sensitivity bit:", dict(zip(vals.tolist(), cnt.tolist())))
vals, cnt = np.unique(recs[ok][:, 6], return_counts=True); print("inhibit bit:", dict(zip(vals.tolist(), cnt.tolist())))
