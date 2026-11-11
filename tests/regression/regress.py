#!/usr/bin/env python3
"""museld picture and sound regression harness.

Renders short reference clips with museld (lossless --write) and compares the
result with the recorded goldens: every decoded frame, the audio stream, the
disc codes read from the VBI and the probe's verdict.  See
docs/regression-testing.md.

    regress.py list                 what the manifest holds
    regress.py fetch [clip...]      download (and verify) the clips
    regress.py run [clip...]        render and compare against the goldens
    regress.py run --accept ...     render and record the result as the golden

Clips and goldens are cached under ~/.cache/museld-regression (or
$MUSELD_REGRESSION_CACHE).  Python 3.11+, numpy, ffmpeg and ffprobe.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import tomllib
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
MANIFEST = HERE / "clips.toml"
GOLDEN_DIR = HERE / "golden"
CACHE = Path(os.environ.get("MUSELD_REGRESSION_CACHE", Path.home() / ".cache" / "museld-regression"))
DEFAULT_MUSELD = REPO / "player" / "cmake-build-relwithdebinfo" / "src" / "museld"

AUDIO_RAW_FORMAT = {"ac3": "ac3", "aac": "adts", "pcm_s16le": "s16le", "pcm_s24le": "s24le",
                    "pcm_s32le": "s32le", "pcm_f32le": "f32le", "flac": "flac"}


class Fail(Exception):
    pass


def log(msg):
    print(msg, flush=True)


def sha256_file(path, limit=None):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def load_manifest():
    with open(MANIFEST, "rb") as f:
        m = tomllib.load(f)
    clips = {c["name"]: c for c in m["clip"]}
    return m, clips


def select_clips(clips, names):
    if not names:
        return list(clips.values())
    out = []
    for n in names:
        matches = [c for c in clips.values() if c["name"] == n or n in c["name"]]
        if not matches:
            raise Fail(f"no clip matches '{n}'")
        out += [c for c in matches if c not in out]
    return out


# --- fetching ---------------------------------------------------------------

def fetch_url(url, dest, expect_sha=None):
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists():
        if expect_sha is None or sha256_file(dest) == expect_sha:
            return dest
        log(f"  {dest.name}: checksum mismatch, fetching again")
        dest.unlink()
    tmp = dest.with_suffix(dest.suffix + ".part")
    log(f"  fetching {url}")
    h = hashlib.sha256()
    with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
        total = r.headers.get("Content-Length")
        done = 0
        while chunk := r.read(1 << 20):
            f.write(chunk)
            h.update(chunk)
            done += len(chunk)
            if total and sys.stdout.isatty():
                print(f"\r    {done / 1e6:.0f} / {int(total) / 1e6:.0f} MB", end="", flush=True)
    if total and sys.stdout.isatty():
        print()
    if expect_sha and h.hexdigest() != expect_sha:
        tmp.unlink()
        raise Fail(f"{url}: sha256 {h.hexdigest()} does not match the manifest's {expect_sha}")
    tmp.rename(dest)
    return dest


def clip_path(manifest, clip):
    return fetch_url(manifest["base_url"] + clip["file"], CACHE / "clips" / clip["file"], clip["sha256"])


# --- rendering --------------------------------------------------------------

def ffprobe_streams(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-show_streams", "-of", "json", str(path)],
                         capture_output=True, text=True, check=True).stdout
    return json.loads(out)["streams"]


def video_frames(path, pix_fmt):
    """All frames of the video stream as raw bytes, plus (w, h, bytes per frame)."""
    streams = ffprobe_streams(path)
    v = next(s for s in streams if s["codec_type"] == "video")
    w, h = v["width"], v["height"]
    bpf = {"yuv420p16le": w * h * 3, "gray16le": w * h * 2}[pix_fmt]
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-map", "0:v", "-f", "rawvideo",
                          "-pix_fmt", pix_fmt, "-"], capture_output=True, check=True).stdout
    if len(raw) % bpf:
        raise Fail(f"{path}: raw video size {len(raw)} is not a whole number of frames")
    return raw, (w, h, bpf), v


def audio_raw(path):
    """The audio stream's bytes, its codec and the byte offset of judge_from seconds (PCM only)."""
    streams = ffprobe_streams(path)
    a = [s for s in streams if s["codec_type"] == "audio"]
    if not a:
        return None, b"", 0
    codec = a[0]["codec_name"]
    bytes_per_second = 0
    if codec.startswith("pcm_s"):
        bytes_per_second = int(a[0]["sample_rate"]) * int(a[0]["channels"]) * int(codec[5:7]) // 8
    fmt = AUDIO_RAW_FORMAT.get(codec)
    if fmt is None:
        raise Fail(f"{path}: no raw format known for audio codec {codec}")
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-map", "0:a:0", "-c", "copy",
                          "-f", fmt, "-"], capture_output=True, check=True).stdout
    return codec, raw, bytes_per_second


CODE_RE = re.compile(r"disc code at input offset (\d+) \([\d.]+ s\): (.*)$")
PROBE_RE = re.compile(r"^(.*): (\S+ .* at \S+ Hz)\s*$")


def parse_log(text):
    codes, probe = [], None
    for line in text.splitlines():
        m = CODE_RE.search(line)
        if m:
            codes.append(f"{m.group(1)} {m.group(2).strip()}")
            continue
        m = PROBE_RE.match(line)
        if m and probe is None and "at " in m.group(2):
            probe = m.group(2)
    return probe, codes


def render(museld, clip_file, args, out_mkv, log_file, timeout):
    cmd = [str(museld), "--no-keys", "--no-sync", "--no-state", "--write", str(out_mkv), "--write-preset", "archival",
           "--log", "M4A2V2D2I2O2", *args, str(clip_file)]
    t0 = time.time()
    with open(log_file, "w") as lf:
        lf.write(" ".join(cmd) + "\n\n")
        lf.flush()
        try:
            p = subprocess.run(cmd, stdout=lf, stderr=subprocess.STDOUT, timeout=timeout)
        except subprocess.TimeoutExpired:
            raise Fail(f"museld did not finish within {timeout} s (log: {log_file})")
    if p.returncode != 0:
        tail = "".join(open(log_file).readlines()[-8:])
        raise Fail(f"museld exited with {p.returncode} (log: {log_file})\n{tail}")
    return time.time() - t0


def measure(mkv, log_file, judge_from):
    raw, (w, h, bpf), v = video_frames(mkv, "yuv420p16le")
    n = len(raw) // bpf
    frame_sha = [hashlib.sha256(raw[i * bpf:(i + 1) * bpf]).hexdigest()[:16] for i in range(n)]
    codec, araw, bps = audio_raw(mkv)
    # The writer conceals the start-up deficit between the first audio and the video clock by a
    # stretch whose length depends on thread timing, so the PCM is judged from judge_from on.
    judged = araw[int(round(judge_from * bps)):] if bps else araw
    probe, codes = parse_log(open(log_file).read())
    num, den = (int(x) for x in v["r_frame_rate"].split("/"))
    return {
        "probe": probe,
        "video": {"width": w, "height": h, "pix_fmt": v["pix_fmt"], "fps": num / den, "frames": n,
                  "sha256": hashlib.sha256(raw).hexdigest(), "frame_sha": frame_sha},
        "audio": {"codec": codec, "bytes": len(araw), "sha256": hashlib.sha256(araw).hexdigest(),
                  "judged_sha256": hashlib.sha256(judged).hexdigest()},
        "codes": codes,
    }


# --- comparing --------------------------------------------------------------

def psnr_compare(test_mkv, golden_mkv, judge_from_frame, max_shift=4):
    """Per-frame PSNR of the Y plane over the judged frames, with the best frame shift."""
    import numpy as np
    a, (w, h, bpf), _ = video_frames(test_mkv, "gray16le")
    b, _, _ = video_frames(golden_mkv, "gray16le")
    A = np.frombuffer(a, dtype="<u2").reshape(-1, h, w).astype(np.float32) / 65535.0
    B = np.frombuffer(b, dtype="<u2").reshape(-1, h, w).astype(np.float32) / 65535.0
    best = None
    for shift in range(-max_shift, max_shift + 1):
        lo = max(judge_from_frame, judge_from_frame + shift)
        n = min(len(A) - lo, len(B) - (lo - shift))
        if n <= 0:
            continue
        ta = A[lo:lo + n]
        tb = B[lo - shift:lo - shift + n]
        mse = np.mean((ta - tb) ** 2, axis=(1, 2))
        psnr = np.where(mse > 0, 10 * np.log10(1.0 / np.maximum(mse, 1e-20)), 99.0)
        cand = (float(psnr.mean()), float(psnr.min()), int(np.argmin(psnr)) + lo, shift, n)
        if best is None or cand[0] > best[0]:
            best = cand
    return best  # (mean, min, worst frame, shift, frames compared)


def audio_compare(test_mkv, golden_mkv, judge_from):
    """RMS of the difference between two PCM streams, in dBFS, or None for non-PCM."""
    import numpy as np
    ca, a, bps = audio_raw(test_mkv)
    cb, b, _ = audio_raw(golden_mkv)
    if ca != cb or not ca.startswith("pcm_s16"):
        return None
    skip = int(round(judge_from * bps))
    A = np.frombuffer(a[skip:], dtype="<i2").astype(np.float32) / 32768
    B = np.frombuffer(b[skip:], dtype="<i2").astype(np.float32) / 32768
    n = min(len(A), len(B))
    if n == 0:
        return None
    d = A[:n] - B[:n]
    rms = float(np.sqrt(np.mean(d * d)))
    return float(20 * np.log10(max(rms, 1e-9))), len(A) - len(B)


def golden_path(clip, stage):
    return GOLDEN_DIR / f"{clip['name']}.{stage}.json"


def golden_mkv(manifest, golden):
    name = golden.get("golden_file")
    if not name:
        return None
    local = CACHE / "golden" / name
    if local.exists():
        return local
    try:
        return fetch_url(manifest["golden_url"] + name, local)
    except Exception as e:  # noqa: BLE001
        log(f"  golden render not available: {e}")
        return None


def compare(manifest, clip, stage, result, mkv, tol_db, audio_tol_db):
    gp = golden_path(clip, stage)
    if not gp.exists():
        return "NEW", "no golden recorded"
    g = json.load(open(gp))
    notes, status = [], "PASS"

    if g["probe"] != result["probe"]:
        notes.append(f"probe '{g['probe']}' -> '{result['probe']}'")
        status = "FAIL"
    if g["codes"] != result["codes"]:
        common = sum(1 for x, y in zip(g["codes"], result["codes"]) if x == y)
        notes.append(f"disc codes differ ({common} of {len(g['codes'])} match, now {len(result['codes'])})")
        status = "FAIL"

    judge_from = clip.get("judge_from", manifest.get("judge_from", 2.0))
    gv, rv = g["video"], result["video"]
    if gv["sha256"] == rv["sha256"]:
        notes.append(f"video identical ({rv['frames']} frames)")
    else:
        first = next((i for i, (x, y) in enumerate(zip(gv["frame_sha"], rv["frame_sha"])) if x != y),
                     min(len(gv["frame_sha"]), len(rv["frame_sha"])))
        same = sum(1 for x, y in zip(gv["frame_sha"], rv["frame_sha"]) if x == y)
        note = f"video differs from frame {first} ({same}/{gv['frames']} identical, now {rv['frames']} frames)"
        gm = golden_mkv(manifest, g)
        if gm is None:
            notes.append(note + ", no golden render to measure against")
            status = "FAIL"
        else:
            p = psnr_compare(mkv, gm, int(round(judge_from * rv["fps"])))
            if p is None:
                notes.append(note + ", too few frames to compare")
                status = "FAIL"
            else:
                mean, mn, worst, shift, n = p
                note += f"; PSNR mean {mean:.1f} dB, min {mn:.1f} dB at frame {worst}"
                if shift:
                    note += f", frames shifted by {shift:+d}"
                notes.append(note)
                if mn < tol_db:
                    status = "FAIL"
                elif status == "PASS":
                    status = "DIFF"

    ga, ra = g["audio"], result["audio"]
    if ga["codec"] != ra["codec"]:
        notes.append(f"audio codec {ga['codec']} -> {ra['codec']}")
        status = "FAIL"
    elif ga["sha256"] == ra["sha256"]:
        notes.append(f"audio identical ({ra['codec']}, {ra['bytes']} bytes)")
    elif ga["judged_sha256"] == ra["judged_sha256"]:
        notes.append(f"audio identical after the warm-up ({ra['codec']}, {ra['bytes']} bytes)")
    else:
        gm = golden_mkv(manifest, g)
        ac = audio_compare(mkv, gm, judge_from) if gm else None
        if ac is None:
            notes.append(f"audio differs ({ra['codec']}, {ga['bytes']} -> {ra['bytes']} bytes)")
            status = "FAIL"
        else:
            rms_db, dlen = ac
            notes.append(f"audio differs: residual {rms_db:.1f} dBFS rms, length {dlen:+d} samples")
            if rms_db > audio_tol_db or dlen:
                status = "FAIL"
            elif status == "PASS":
                status = "DIFF"
    return status, "; ".join(notes)


# --- commands ---------------------------------------------------------------

def cmd_list(manifest, clips, args):
    for c in clips.values():
        stages = c.get("stages", manifest["standard_stages"][c["standard"]])
        have = [s for s in stages if golden_path(c, s).exists()]
        print(f"{c['name']:40s} {c['standard']:5s} stages {','.join(stages)}  goldens {len(have)}/{len(stages)}")
        print(f"    {c['why']}")


def cmd_fetch(manifest, clips, args):
    for c in select_clips(clips, args.clips):
        p = clip_path(manifest, c)
        log(f"  {c['name']}: {p} ({p.stat().st_size / 1e6:.0f} MB)")


def cmd_run(manifest, clips, args):
    museld = Path(args.museld)
    if not museld.exists():
        raise Fail(f"museld binary not found: {museld} (build it, or pass --museld)")
    run_dir = CACHE / "runs" / time.strftime("%Y%m%d-%H%M%S")
    run_dir.mkdir(parents=True)
    stages_all = manifest["stages"]
    results = []
    for c in select_clips(clips, args.clips):
        clip_file = clip_path(manifest, c)
        stages = c.get("stages", manifest["standard_stages"][c["standard"]])
        if args.stage:
            stages = [s for s in stages if s in args.stage]
        for stage in stages:
            tag = f"{c['name']}.{stage}"
            mkv = run_dir / f"{tag}.mkv"
            logf = run_dir / f"{tag}.log"
            stage_args = stages_all[stage]
            try:
                secs = render(museld, clip_file, [*stage_args, *c.get("args", [])], mkv, logf, args.timeout)
                result = measure(mkv, logf, c.get("judge_from", manifest.get("judge_from", 2.0)))
                if args.accept:
                    status, note = accept(manifest, c, stage, result, mkv, [*stage_args, *c.get("args", [])], args)
                else:
                    status, note = compare(manifest, c, stage, result, mkv, args.tolerance, args.audio_tolerance)
                note = f"{secs:.1f} s; " + note
            except Fail as e:
                status, note = "FAIL", str(e)
            results.append((tag, status, note))
            log(f"{status:4s} {tag}: {note}")
    failed = [r for r in results if r[1] == "FAIL"]
    log(f"\n{len(results)} renders in {run_dir}: {len(failed)} failed, "
        f"{sum(1 for r in results if r[1] == 'DIFF')} within tolerance, "
        f"{sum(1 for r in results if r[1] == 'NEW')} without golden")
    if args.keep_renders or failed:
        log(f"renders kept in {run_dir}")
    else:
        shutil.rmtree(run_dir)
    return 1 if failed else 0


def accept(manifest, clip, stage, result, mkv, used_args, args):
    name = f"{clip['name']}.{stage}.{result['video']['sha256'][:12]}.mkv"
    dest = CACHE / "golden" / name
    dest.parent.mkdir(parents=True, exist_ok=True)
    fresh = not dest.exists()  # the name carries the video hash: an existing file is the same render
    if fresh:
        shutil.copyfile(mkv, dest)
    golden = {"clip": clip["name"], "stage": stage, "museld_args": used_args,
              "recorded": time.strftime("%Y-%m-%d"), "golden_file": name, **result}
    gp = golden_path(clip, stage)
    old = json.load(open(gp)) if gp.exists() else None
    with open(gp, "w") as f:
        json.dump(golden, f, indent=1)
        f.write("\n")
    uploaded = False
    if args.upload and fresh:
        r = subprocess.run(["rsync", "-a", str(dest), args.upload.rstrip("/") + "/"])
        uploaded = r.returncode == 0
        if not uploaded:
            log(f"  upload of {name} failed (rsync exit {r.returncode}); the golden is recorded, upload it later")
    if old and old["video"]["sha256"] == result["video"]["sha256"] and \
            old["audio"].get("judged_sha256") == result["audio"]["judged_sha256"]:
        return "PASS", "golden unchanged"
    return "NEW", f"golden recorded as {name}" + (" and uploaded" if uploaded else "")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    f = sub.add_parser("fetch")
    f.add_argument("clips", nargs="*")
    r = sub.add_parser("run")
    r.add_argument("clips", nargs="*", help="clip names (substrings match); default all")
    r.add_argument("--stage", action="append", help="only these stages (repeatable)")
    r.add_argument("--museld", default=str(DEFAULT_MUSELD))
    r.add_argument("--accept", action="store_true", help="record the renders as the new goldens")
    r.add_argument("--upload", metavar="RSYNC_DEST", help="with --accept: rsync the golden renders there")
    r.add_argument("--tolerance", type=float, default=45.0, help="minimum per-frame PSNR in dB to pass (default 45)")
    r.add_argument("--audio-tolerance", type=float, default=-70.0,
                   help="maximum PCM residual in dBFS rms to pass (default -70)")
    r.add_argument("--timeout", type=float, default=600.0)
    r.add_argument("--keep-renders", action="store_true")
    args = ap.parse_args()
    manifest, clips = load_manifest()
    try:
        rc = {"list": cmd_list, "fetch": cmd_fetch, "run": cmd_run}[args.cmd](manifest, clips, args)
    except Fail as e:
        log(f"error: {e}")
        return 2
    return rc or 0


if __name__ == "__main__":
    sys.exit(main())
