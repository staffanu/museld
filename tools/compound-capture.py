#!/usr/bin/env python3
# Joins raw 16-bit captures into one file with a no-carrier gap between them, to play a disc
# change through museld's live input: the player's output while a disc is swapped is noise
# at a small fraction of the RF level, synthesised here as Gaussian noise at 1 % of the first
# file's rms around its mean.  All inputs must share the sample rate and the s16 format
# (ldconv converts, with -n for a stretch: ldconv -n 20s -r 40e6 side.ldf side-20s.s16).
#
#   compound-capture.py OUT RATE GAP_SECONDS FILE1 FILE2 [FILE3 ...]
#
# then, for the UDP path with the ethadc stream tools (the ethadc repository, stream/):
#
#   museld udp://127.0.0.1:5000 &
#   ethadc-tx --dest 127.0.0.1:5000 --sample-rate 40M --format s16 --throttle --jumbo OUT
#
# (--jumbo on a Mac: at 1440-byte payloads the loopback loses a tenth of the packets, the
# receive buffer being capped at 8 MiB by kern.ipc.maxsockbuf), or for the pipe path:
#
#   cat OUT | museld --input-format s16 --sample-freq 40e6 -
import sys, numpy as np
out, rate, gap = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]); files = sys.argv[4:]
rng = np.random.default_rng(1)
with open(out, 'wb') as o:
    for i, f in enumerate(files):
        x = np.memmap(f, np.int16, 'r')
        if i == 0:
            head = np.asarray(x[:10_000_000]).astype(np.float64); mean, rms = head.mean(), head.std()
            print(f'{f}: mean {mean:.0f} rms {rms:.0f}; gap noise sigma {0.01*rms:.0f}')
        for k in range(0, len(x), 50_000_000):
            o.write(np.asarray(x[k:k + 50_000_000]).tobytes())
        if i < len(files) - 1:
            n = int(gap * rate)
            for k in range(0, n, 50_000_000):
                m = min(50_000_000, n - k)
                o.write((mean + 0.01 * rms * rng.standard_normal(m)).astype(np.int16).tobytes())
        print(f'{f}: {len(x)/rate:.1f} s', flush=True)
