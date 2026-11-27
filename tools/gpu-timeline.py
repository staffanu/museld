#!/usr/bin/env python3
"""GPU timeline analysis of a museld run logged with MUSELD_GPU_TIMELINE=1 (stderr and the --log P3
lines in one file; see player/src/musevk/GpuTimeline.h).

  gpu-timeline.py LOG summary [BIN_S]      per-bin GPU occupancy, per-label GPU time, queue waits, underruns
  gpu-timeline.py LOG detail T0 T1         every submission with decode-second T0..T1 (seconds after the first record)
  gpu-timeline.py LOG dips [HALF_S]        the detail of HALF_S s around every underrun line
"""
import sys, re, datetime, collections

def parse(path):
    recs = []           # dicts: label, sub, wait, done (us cpu), gb, ge (ns gpu)
    events = []         # (cpu_us, text) from stamped log lines of interest
    epoch_wall = None; epoch_cpu = None
    stamp_re = re.compile(r'^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+) \[(\w+) *\] \((\w+)\): (.*)$')
    for line in open(path, errors='replace'):
        line = line.rstrip('\n')
        if line.startswith('gpu epoch '):
            m = re.match(r'gpu epoch (\S+ \S+) is (\d+) us after the first submit', line)
            epoch_wall = datetime.datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S.%f'); epoch_cpu = int(m.group(2))
            continue
        if line.startswith('gpu '):
            f = line.split()
            recs.append(dict(label=f[1], sub=int(f[3]), wait=int(f[5]), done=int(f[7]), gb=int(f[9]), ge=int(f[11])))
            continue
        m = stamp_re.match(line)
        if m and epoch_wall is not None:
            t = datetime.datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S.%f')
            cpu = (t - epoch_wall).total_seconds() * 1e6 + epoch_cpu
            txt = m.group(4)
            if 'underrun' in txt or 'Thermal' in txt or 'demodulate avg' in txt:
                events.append((cpu, m.group(3), txt))
    # align the GPU clock to the CPU clock: the submission that started soonest after its submit
    off = min(r['gb'] / 1000 - r['sub'] for r in recs)
    for r in recs:
        r['g0'] = r['gb'] / 1000 - off; r['g1'] = r['ge'] / 1000 - off
        r['qwait'] = r['g0'] - r['sub']; r['span'] = r['g1'] - r['g0']; r['lat'] = r['done'] - r['g1']
    return recs, events, off

def union_busy(ivs, lo, hi):
    ivs = sorted((max(a, lo), min(b, hi)) for a, b in ivs if b > lo and a < hi)
    busy = 0; gaps = []; cur = None
    for a, b in ivs:
        if cur is None: cur = [a, b]
        elif a > cur[1]: busy += cur[1] - cur[0]; gaps.append(a - cur[1]); cur = [a, b]
        else: cur[1] = max(cur[1], b)
    if cur: busy += cur[1] - cur[0]
    return busy, gaps

def summary(recs, events, bin_s):
    t0 = recs[0]['sub']; tend = max(r['done'] for r in recs)
    nb = int((tend - t0) / 1e6 / bin_s) + 1
    bins = [collections.defaultdict(list) for _ in range(nb)]
    for r in recs:
        bins[int((r['g0'] - t0) / 1e6 / bin_s)][r['label']].append(r)
    ev = collections.defaultdict(list)
    for cpu, cat, txt in events:
        i = int((cpu - t0) / 1e6 / bin_s)
        if 0 <= i < nb:
            ev[i].append('UNDERRUN' if 'underrun' in txt else txt[:40])
    print(f"{'sec':>6} {'busy%':>5} {'demod':>6} {'dec1':>6} {'dec2':>6} {'show':>5} {'maxgap':>6} "
          f"{'qw-dem':>6} {'qw-dec':>6} {'sp-dem':>6} {'sp-dec2':>7} {'lat-dem':>7} {'n-dem':>5} {'n-dec':>5}  events")
    for i, b in enumerate(bins):
        lo = t0 + i * bin_s * 1e6; hi = lo + bin_s * 1e6
        allr = [r for rs in b.values() for r in rs]
        if not allr: continue
        busy, gaps = union_busy([(r['g0'], r['g1']) for r in allr], lo, hi)
        def tot(l): return sum(r['span'] for r in b.get(l, [])) / 1000 / bin_s
        def mx(l, k): return max([r[k] for r in b.get(l, [])] or [0]) / 1000
        print(f"{i*bin_s:6.0f} {100*busy/(hi-lo):5.1f} {tot('demod'):6.2f} {tot('dec1'):6.2f} {tot('dec2'):6.2f} {tot('show'):5.2f} "
              f"{max(gaps or [0])/1000:6.1f} {mx('demod','qwait'):6.1f} {max(mx('dec1','qwait'), mx('dec2','qwait')):6.1f} "
              f"{mx('demod','span'):6.2f} {mx('dec2','span'):7.2f} {mx('demod','lat'):7.1f} {len(b.get('demod',[])):5d} {len(b.get('dec1',[])):5d}  "
              + ' '.join(ev.get(i, [])))

def detail(recs, events, a, b):
    t0 = recs[0]['sub']
    rows = [(r['sub'], 'r', r) for r in recs if a <= (r['sub'] - t0) / 1e6 <= b]
    rows += [(cpu, 'e', (cat, txt)) for cpu, cat, txt in events if a <= (cpu - t0) / 1e6 <= b]
    rows.sort(key=lambda x: x[0])
    prev_end = None
    print(f"{'t(s)':>10} {'label':5} {'sub':>8} {'g0':>8} {'g1':>8} {'done':>8} {'qwait':>6} {'span':>6} {'lat':>6} {'gap':>6}")
    for t, kind, x in rows:
        if kind == 'e':
            print(f"{(t-t0)/1e6:10.3f} ---- {x[0]}: {x[1]}")
            continue
        r = x
        gap = (r['g0'] - prev_end) / 1000 if prev_end is not None else 0
        prev_end = max(prev_end or 0, r['g1'])
        rel = lambda v: (v - t0) / 1000  # ms
        print(f"{(r['sub']-t0)/1e6:10.3f} {r['label']:5} {rel(r['sub']):8.1f} {rel(r['g0']):8.1f} {rel(r['g1']):8.1f} {rel(r['done']):8.1f} "
              f"{r['qwait']/1000:6.2f} {r['span']/1000:6.2f} {r['lat']/1000:6.2f} {gap:6.2f}")

if __name__ == '__main__':
    recs, events, off = parse(sys.argv[1])
    mode = sys.argv[2] if len(sys.argv) > 2 else 'summary'
    print(f"# {len(recs)} submissions, gpu-cpu clock offset {off/1000:.3f} ms, {sum('underrun' in e[2] for e in events)} underrun lines")
    if mode == 'summary':
        summary(recs, events, float(sys.argv[3]) if len(sys.argv) > 3 else 1.0)
    elif mode == 'detail':
        detail(recs, events, float(sys.argv[3]), float(sys.argv[4]))
    elif mode == 'dips':
        half = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5
        t0 = recs[0]['sub']; last = -1e9
        for cpu, cat, txt in events:
            if 'underrun' in txt and cpu - last > 2 * half * 1e6:
                last = cpu; c = (cpu - t0) / 1e6
                print(f"\n==== underrun at {c:.3f} s"); detail(recs, events, c - half, c + half)
