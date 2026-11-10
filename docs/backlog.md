# Developer backlog

Work worth doing, in rough order of value, with the reasoning behind each item so it can be
picked up cold.  This is a working list for development, not the list of known limitations in
`museld.md`: nothing here is promised, and items leave when done or when they stop making sense.
Last revised 2026-11-09.

## Housekeeping

- **Remove the abseil workaround in CI** once MSYS2 rebuilds `mingw-w64-ucrt-x86_64-onnxruntime`
  against abseil 20260817 (or later).  The step "Work around onnxruntime linked against an older
  abseil" in `.github/workflows/ci.yml` prints "this workaround step can be removed" when that
  has happened.  Background: MSYS2 moved abseil-cpp on 2026-10-29 without rebuilding onnxruntime
  1.29.0-2, and the full Windows package's museld.exe failed to load.
- **Try live type switching on the real udp:// ethadc stream.**  The stdin path is tested at 40
  and 62.5 MHz (NTSC/PAL/MUSE in either direction, 1-3 s per switch); the decoder box is not.
  Judge the "NO SIGNAL" pacing on a real disc change, and whether three agreeing chunks is the
  right count.
- **EFM-only PAL discs play silent** with the default analog track (the Stu samples).  An automatic
  fallback to EFM when the analog carriers are absent would fix it; today it needs `--efm`.
- **PAL CAV picture stop** has not been checked on a PAL CAV disc.
- **muse-16 (16.2 MHz baseband) input type** is legacy: keep it working for existing u16 files,
  no new formats or level work, remove when nothing needs it.
- **Settings changed with keys do not carry to the next input** on a multi-file command line
  (tint, black level, audio track, subtitle track, CX).  Options do.  A per-capture sidecar
  (below) would also cover this.

## Picture quality

- **MUSE motion detection and reconstruction filters.**  Both were done early and are simple;
  MUSE quality is dominated by the still/moving decision and the reconstruction filters far more
  than NTSC is.  The motion-vector investigation (2026-10) closed off the other avenue: a
  client-side recompute gives no gain.  The regression set has two MUSE clips to iterate against.
- **Noise reduction.**  There is no temporal or spatial luma/chroma NR as such; the 3D comb only
  separates Y/C on still areas.  PAL noise is 2.4x NTSC's and worn-disc captures are noisy.  A
  motion-gated temporal NR reusing the existing motion mask is the cheapest visible gain on SD.
- **NTSC deinterlacing in moving areas** is plain intra-field; edge-directed interpolation would
  sharpen diagonals.
- **PAL calibration.**  Calibrated on the GGV1011 test disc and a handful of captures; the
  dropout detector's thresholds were calibrated on NTSC; Hanover bars under heavy noise are worth
  measuring with the pal-pilot tooling.
- **MUSE open questions:** the non-linear de-emphasis on baseband input is unresolved; the
  equaliser's reference could be verified against the Sony test disc's patterns; the LD versus BS
  MUSE difference is unknown.

## Functionality

- **Per-capture remembered settings:** a sidecar file next to the input, written on quit and
  read on open (tint, black level, aspect, overscan, audio track, subtitle track, CX).
- **Player-like navigation:** frame search by CAV frame number and time search by CLV time code
  with numeric entry (the chapter probe infrastructure does the hard part), step backwards,
  slow motion, fast scan with picture.
- **Richer `--write`:** all audio tracks as separate streams, disc chapters as chapter markers,
  closed captions as a subtitle stream.
- **AC3 bitstream passthrough** to the audio device for an AV receiver, instead of decoding to
  5.1 PCM.  Natural for the decoder-box setup.
- **Direct USB capture input** (Domesday Duplicator, Picoscope) without an external program.
- **OCR polish:** a presentation-delay buffer would remove the one-second subtitle lag and allow
  inpainting the burned-in text away; in-flight translations should be flushed at exit.

## Done since this list was started

- 2026-11-05: picture and sound regression harness (`tests/regression/`).
- 2026-11-06: live NTSC/PAL/MUSE type switching on a live input.
- 2026-11-09: `--write` keeps one file across the inputs that name it (the sides of a disc).
- 2026-11-10: EFM robustness merged: sync-anchored timing loop, confirmed re-framing, flagged
  start-up, opt-in gated C1 (`--c1-strategy`), the symbol and concealment dumps and `ldstack`.
