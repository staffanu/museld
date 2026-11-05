# Picture and sound regression tests

`tests/regression/regress.py` renders short reference clips with museld and compares
every decoded frame, the audio stream, the disc codes read from the VBI and the probe's
verdict against recorded goldens. It exists so that a change to a filter, the timebase,
the dropout detector or the audio path shows up as a measured difference on known
material rather than as an impression from a single frame.

```
tests/regression/regress.py list                 # the clips, their stages, which goldens exist
tests/regression/regress.py fetch                # download and verify all clips (about 1.5 GB)
tests/regression/regress.py run                  # render everything and compare (10 minutes or so)
tests/regression/regress.py run --stage default  # one render per clip
tests/regression/regress.py run pal- wow         # clips whose names contain these
```

It needs Python 3.11 or later, numpy, `ffmpeg` and `ffprobe`, and a museld built with
FFmpeg (`--write`); by default it runs `player/cmake-build-relwithdebinfo/src/museld`
(`--museld` names another). Clips, golden renders and the renders of each run are
cached under `~/.cache/museld-regression` (`MUSELD_REGRESSION_CACHE` moves it).

## What is compared

Each clip is rendered with `--no-keys --no-sync --write --write-preset archival`
(lossless FFV1 at 16 bits and the audio as PCM, or the AC3 bitstream for the AC3
track), once per *stage*. A stage is a set of museld options that isolates part of the
decode, so a difference can be placed: `default`, `intra-field` (no motion-adaptive
interpolation), `no-3d-comb`, `no-film-mode`, `no-dropout` and `eq-off` (MUSE). The
manifest lists which stages each standard gets.

From the render the harness records, in `tests/regression/golden/<clip>.<stage>.json`:

- a hash of every decoded frame and of the whole video stream;
- a hash of the audio stream, with its codec and length;
- every change of the disc code with the input offset it was read at (the `M4` log);
- the probe's verdict (format, type, rate).

The golden JSON is small and lives in the repository. The golden render itself (the
.mkv, 50 to 250 MB per stage) is kept outside it, named by the video hash so the two
cannot drift apart, and is only needed when a render no longer hashes the same.

A comparison has four outcomes:

- **PASS**: everything hashes identically. On the machine that recorded the golden this
  is the normal result: museld's output is bit-exact run to run (the GPU shaders are
  deterministic), so any change at all in the picture is caught.
- **DIFF**: the frames differ but every judged frame is within tolerance of the golden
  render (per-frame PSNR of the luma plane at least 45 dB, `--tolerance`), or the PCM
  audio differs by less than -70 dBFS rms (`--audio-tolerance`). This is what another GPU
  or another CPU's floating point produces; the report says where the worst frame is and
  whether the frames are shifted.
- **FAIL**: a frame is outside the tolerance, the audio differs by more or in length, the
  disc codes or the probe's verdict changed, or museld failed. The renders of the run are
  kept and the log of each render is beside them.
- **NEW**: no golden for this clip and stage yet.

The first `judge_from` seconds of a clip (2 s by default, set per clip) are the decoder's
warm-up and are hashed but not judged: sync lock, AGC, the temporal comb's window, the
cadence trackers and the MUSE equaliser all need a run-in, and the golden would otherwise
pin their transient. The PCM audio is judged from the same point: the analog track's CX
expansion starts at the first frame with a readable VBI code, so a change in when the
decoder first reads one moves the start of the expanded sound. (The first version of the
harness found the CX switch-on point varying from run to run, which turned out to be a
thread race in the decoder, since fixed; the rule stays for the warm-up proper.) The video
is not affected, so a frame hash mismatch anywhere, warm-up included, is reported.

## Updating a golden

A change that is meant to alter the picture is accepted by re-recording:

```
tests/regression/regress.py run --accept [clips...] [--stage ...] \
    --upload madeye.org:/nas/video/LD-Captures/public/museld-testdata/golden
```

This writes the JSON files (commit them with the change that caused the difference, so
the history says why each golden moved) and copies the renders to the local cache and,
with `--upload`, to the server by rsync. Accept only after looking at what changed:
run without `--accept` first and read the PSNR and the worst frame, and export that frame
from both renders (`ffmpeg -i x.mkv -vf select=eq(n\,N) -frames 1 x.png`) when the number
alone does not say whether it got better.

## The clips

The clips are cuts of RF captures, 4 s each (one is the whole 6.7 s capture, one is
ld-decode's 0.75 s pattern), compressed as `.ldf` with ldconv:

```
ldconv -s 195s -n 4s capture.ogg ntsc-ggv1069-bars-195s.ldf
```

They are served from `https://madeye.org/ld-captures/public/museld-testdata/`, which is
`/nas/video/LD-Captures/public/museld-testdata/` on multivac, with a `SHA256SUMS` file
beside them; the manifest `tests/regression/clips.toml` carries each clip's hash, the
standard, extra museld options (`--ac3`, `--efm` for the EFM-only PAL discs), the source
capture and position, and the reason it is in the set. The set covers the cases the
decoder was calibrated or fixed on: the NTSC and PAL test discs' colour bars, a clean NTSC
film, two AC3 discs, a CAV disc with heavy eccentricity on a static text frame, repeated
frames from a player kicking back, the same disc at 30 and 40 MHz, PAL film with a 2:2
cadence, a PAL capture whose baseband wanders, PAL video with real dropouts and bright
detail, and two MUSE discs. `regress.py list` prints the reasons.

To add a clip: cut it, copy it to the directory on multivac, append its line to
`SHA256SUMS` there, add a `[[clip]]` entry to the manifest, run with `--accept` for that
clip and commit the manifest and the new golden files together.

Not covered yet: a DTS disc and a closed-caption disc (no captures exist here), MUSE
baseband (`muse-16`) input, and the live-input paths (pipe, http, udp).
