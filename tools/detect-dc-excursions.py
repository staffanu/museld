#!/usr/bin/env python3
"""
Scan raw laserdisc RF captures for the "DC baseline excursion" fault:
a transient overdrives the AC-coupled scope input, the coupling capacitor
gets kicked, and the signal pins near an ADC rail while it recovers with a
~100 ms RC time constant.  On a healthy capture the DC baseline (averaged
over ~1 ms, long enough to average out the RF carrier) sits near zero; during
an event it swings to a large fraction of full scale and the ADC clips heavily.

The scan is one streaming pass over the file, so it is disk-bound rather than
CPU-bound -- a C version would not be meaningfully faster.

Usage:
    detect-dc-excursions.py [options] FILE [FILE ...]

    --bits 8|16          sample width (default 16, little-endian signed)
    --rate HZ            sample rate, only used to print durations (default 62.5e6)
    --block N            averaging block in samples (default 62500 ~= 1 ms @ 62.5 MS/s)
    --dc-threshold V     |block DC| above this counts as an excursion (default 4000)
    --clip-level V       |sample| at/above this counts as clipped (default 32700 for 16-bit)
    --min-duration-ms M  ignore excursions shorter than this (default 20)
    --merge-gap-ms M     merge excursions separated by less than this (default 30)
    --start-sample S     start scanning at this sample offset (default 0)
    --max-samples N      scan at most N samples (default 0 = to end of file)
    --csv PATH           append/write one row per event as CSV
    --profile PATH.npz   also dump the full per-block DC/clip trace of the LAST
                         file scanned, for plotting

Events are printed to stdout; a per-file count goes to stderr.  Exit status is
1 if any event was found, 0 otherwise (handy in scripts / CI-style checks).
"""

import sys
import os
import argparse
import numpy as np


def scan_file(path, bits, block, clip_level, start_sample, max_samples, progress=True):
    """Stream the file and return per-block (dc_mean, clip_fraction) arrays."""
    dtype = np.dtype('<i1') if bits == 8 else np.dtype('<i2')
    bytes_per = dtype.itemsize
    file_bytes = os.path.getsize(path)

    start_byte = start_sample * bytes_per
    if start_byte >= file_bytes:
        return np.array([], np.float32), np.array([], np.float32), start_sample

    avail_samples = (file_bytes - start_byte) // bytes_per
    if max_samples:
        avail_samples = min(avail_samples, max_samples)

    chunk_blocks = 256                       # 256 * 1 ms = 256 ms of data per read
    chunk_samples = chunk_blocks * block
    dc_parts, clip_parts = [], []
    done = 0

    with open(path, 'rb') as fh:
        fh.seek(start_byte)
        while done < avail_samples:
            want = min(chunk_samples, avail_samples - done)
            want -= want % block             # whole blocks only
            if want == 0:
                break
            buf = fh.read(want * bytes_per)
            got = len(buf) // bytes_per
            got -= got % block
            if got == 0:
                break
            a = np.frombuffer(buf, dtype=dtype, count=got).reshape(-1, block)
            dc_parts.append(a.mean(axis=1).astype(np.float32))
            # compare on the native dtype to avoid abs() overflow at the rail
            clipped = (a >= clip_level) | (a <= -clip_level)
            clip_parts.append(clipped.mean(axis=1).astype(np.float32))
            done += got
            if progress and file_bytes:
                pct = 100.0 * (start_byte + done * bytes_per) / file_bytes
                sys.stderr.write("\r  %s: %5.1f%%" % (os.path.basename(path), pct))
                sys.stderr.flush()

    if progress:
        sys.stderr.write("\r" + " " * 40 + "\r")
    dc = np.concatenate(dc_parts) if dc_parts else np.array([], np.float32)
    clip = np.concatenate(clip_parts) if clip_parts else np.array([], np.float32)
    return dc, clip, start_sample


def find_events(dc, clip, block, base_sample, dc_thresh, min_blocks, gap_blocks):
    """Group above-threshold blocks into events, merging small gaps."""
    above = np.abs(dc) > dc_thresh
    idx = np.flatnonzero(above)
    if idx.size == 0:
        return []

    # split into runs wherever the gap between flagged blocks exceeds gap_blocks
    splits = np.flatnonzero(np.diff(idx) > gap_blocks) + 1
    events = []
    for run in np.split(idx, splits):
        b0, b1 = run[0], run[-1]
        if (b1 - b0 + 1) < min_blocks:
            continue
        seg = np.abs(dc[b0:b1 + 1])
        pk = b0 + int(np.argmax(seg))
        events.append({
            'start_sample': base_sample + b0 * block,
            'peak_sample':  base_sample + pk * block,
            'end_sample':   base_sample + (b1 + 1) * block,
            'peak_dc':      float(dc[pk]),
            'peak_clip':    float(clip[b0:b1 + 1].max()),
            'dur_samples':  (b1 - b0 + 1) * block,
        })
    return events


def main():
    p = argparse.ArgumentParser(add_help=True, description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('files', nargs='+')
    p.add_argument('--bits', type=int, default=16, choices=(8, 16))
    p.add_argument('--rate', type=float, default=62.5e6)
    p.add_argument('--block', type=int, default=62500)
    p.add_argument('--dc-threshold', type=float, default=4000)
    p.add_argument('--clip-level', type=int, default=None)
    p.add_argument('--min-duration-ms', type=float, default=20)
    p.add_argument('--merge-gap-ms', type=float, default=30)
    p.add_argument('--start-sample', type=float, default=0)
    p.add_argument('--max-samples', type=float, default=0)
    p.add_argument('--csv', default=None)
    p.add_argument('--profile', default=None)
    args = p.parse_args()

    clip_level = args.clip_level if args.clip_level is not None else (120 if args.bits == 8 else 32700)
    ms_per_block = 1000.0 * args.block / args.rate
    min_blocks = max(1, round(args.min_duration_ms / ms_per_block))
    gap_blocks = max(1, round(args.merge_gap_ms / ms_per_block))

    csv = None
    if args.csv:
        new = not os.path.exists(args.csv)
        csv = open(args.csv, 'a')
        if new:
            csv.write("file,event,start_sample,peak_sample,end_sample,peak_dc,peak_clip_pct,dur_samples,dur_ms\n")

    total_events = 0
    dc = clip = None
    for path in args.files:
        dc, clip, base = scan_file(path, args.bits, args.block, clip_level,
                                   int(args.start_sample), int(args.max_samples))
        events = find_events(dc, clip, args.block, base,
                             args.dc_threshold, min_blocks, gap_blocks)
        total_events += len(events)

        if events:
            print("%s: %d excursion(s)" % (path, len(events)))
            print("  %-4s %14s %14s %14s %9s %8s %9s" %
                  ("#", "start", "peak", "end", "peak_dc", "clip%", "dur_ms"))
            for i, e in enumerate(events):
                dur_ms = 1000.0 * e['dur_samples'] / args.rate
                print("  %-4d %14d %14d %14d %9.0f %7.1f %9.1f" %
                      (i, e['start_sample'], e['peak_sample'], e['end_sample'],
                       e['peak_dc'], e['peak_clip'] * 100, dur_ms))
                if csv:
                    csv.write("%s,%d,%d,%d,%d,%.0f,%.1f,%d,%.1f\n" %
                              (path, i, e['start_sample'], e['peak_sample'], e['end_sample'],
                               e['peak_dc'], e['peak_clip'] * 100, e['dur_samples'], dur_ms))
        else:
            print("%s: clean" % path)
        sys.stderr.write("  -> %d event(s) in %s\n" % (len(events), os.path.basename(path)))

    if csv:
        csv.close()
    if args.profile and dc is not None and dc.size:
        base = int(args.start_sample)
        np.savez_compressed(args.profile,
                            sample=base + np.arange(dc.size, dtype=np.int64) * args.block,
                            dc=dc, clip=clip)
        sys.stderr.write("wrote per-block profile of last file to %s\n" % args.profile)

    return 1 if total_events else 0


if __name__ == '__main__':
    sys.exit(main())
