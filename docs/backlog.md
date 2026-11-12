# Developer backlog

Work worth doing, in rough order of value, with the reasoning behind each item so it can be
picked up cold.  This is a working list for development, not the list of known limitations in
`museld.md`: nothing here is promised, and items leave when done or when they stop making sense.
Last revised 2026-11-12.

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
- **muse-16 (16.2 MHz baseband) input type** is legacy: keep it working for existing u16 files,
  no new formats or level work, remove when nothing needs it.
- **Settings changed with keys do not carry to the next input** on a multi-file command line
  (tint, black level, audio track, subtitle track, CX).  Options do.  The per-disc memory does
  not cover it either: it brings a setting back for the same capture, and the next input is
  another file, usually the disc's other side.  A per-capture sidecar (below) would not cover
  it either, for the same reason; what would is carrying the player state from one input to
  the next within a run where the inputs decode alike.
- **MUSE audio: the sync search checks for the pattern it has just consumed.**  When the search
  in `AudioDecoder::decodeFrame` finds the sync pattern with less than a frame of bits after it,
  it removes the pattern and leaves the loop with the failure count at zero; the next call then
  takes the locked branch and tests the first 16 bits of the queue, which are the control
  signal, not a sync.  That costs three "Sync pattern not recognized!" warnings, two audio
  frames decoded 16 bits out of place (their control words also go into the mode vote) and a
  second search before the lock holds.  It heals by itself, so it is audible only as a slightly
  longer gap after a seek or a dropout.  The fix is to remember that the sync of the next frame
  is already consumed (or to leave the pattern in the queue until the frame is complete); the
  first frames of audio after start-up may move, so expect to re-record the MUSE goldens.

## Picture quality

- **MUSE motion detection and reconstruction filters.**  Both were done early and are simple;
  MUSE quality is dominated by the still/moving decision and the reconstruction filters far more
  than NTSC is.  The motion-vector investigation (2026-10) closed off the other avenue: a
  client-side recompute gives no gain.  The regression set has two MUSE clips to iterate against.
- **Noise reduction, spatial.**  The temporal NR (`--nr`, key 6, 2026-11-11) averages still parts
  only; moving parts and PAL's 2.4x noise on them would need a spatial or motion-compensated
  stage, which is a different order of work.
- **Edge-template deringing of NTSC luma.**  Two artefacts sit next to every hard edge of some
  discs and are tied to the edge, not to the channel.  A dip 150-250 ns before the edge, mirrored
  on the two polarities (a linear-phase lowpass with a cutoff around 4-5 MHz and a first lobe of
  3-4% of the step, in the post-production chain: the VBI code pulses of the same frames come
  through flat within 1 IRE, so the disc, player and museld's filters are clean, and the pulses
  are too slow, 260 ns rise against the picture's 150, to train anything above 3 MHz); and a
  bright halo of 3-9 IRE for about 1 us after white-to-black edges, the pre-emphasis undershoot
  clipped at the sync tip at mastering.  No linear filter removes either: inverting the source
  lowpass boosts a band the channel has already emptied, and softening the video lowpass (what
  ld-decode's Butterworth does) only trades it against sharpness.  The adaptive fix: estimate a
  residual template per disc by averaging the signal around detected edges relative to their
  plateaus, normalised by step height (zero on a disc without the source filter), and subtract it
  at every edge scaled by that edge's height at its sub-sample crossing; the halo gets a template
  conditioned on step height, since it only appears when the undershoot clips.  The edge itself
  is left alone, so nothing softens.  Its limit is a real 200 ns line of a few IRE beside an
  edge, told apart only by not being edge-correlated over the frame.  Seen on black text on a
  white card (2026-11).  Tried offline on that frame (2026-11-11), three model levels: a
  per-edge template outside the transition (the dip fell from 5.0% to 3.6% of white), the
  measured edge response minus a clean step over the whole edge (3.7%), and a per-line step
  fit with joint least squares of the heights and per-edge softness (4.2%).  None cleared it,
  because the glyphs are anti-aliased by their character generator and so softer than the
  card's own edges: the ringing they carry is the same chain's, but smeared and shifted, and a
  response learned from the sharp edges misfits them on the foot of the edge rather than on
  the lobe.  A joint non-linear fit of position, width and height per edge might, but not at
  real-time cost and not safely on noisy film; the practical lever stays a softer video lowpass
  as a per-disc option.  Parked.
- **PAL calibration.**  Calibrated on the GGV1011 test disc and a handful of captures; the
  dropout detector's thresholds were calibrated on NTSC; Hanover bars under heavy noise are worth
  measuring with the pal-pilot tooling.
- **MUSE open questions:** the non-linear de-emphasis on baseband input is unresolved; the
  equaliser's reference could be verified against the Sony test disc's patterns; the LD versus BS
  MUSE difference is unknown.

## Functionality

- **Per-capture remembered settings:** a sidecar file next to the input, written on quit and
  read on open, for what the state file remembers (aspect, overscan, audio track, subtitle
  tracks, black level, CX) plus tint, which first needs to become a live setting (it is set at
  the decoder's construction, from `--tint` only).
- **Player-like navigation:** frame search by CAV frame number and time search by CLV time code
  with numeric entry (the chapter probe infrastructure does the hard part), step backwards,
  slow motion, fast scan with picture.
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
- 2026-11-11: discs without analog audio carriers switch to the EFM track by themselves
  (`--analog` keeps the analog track).
- 2026-11-11: motion-gated temporal noise reduction on SD (`--nr off|low|high`, key 6, the
  menu's Picture page), off by default.
- 2026-11-11: the PAL CAV picture stop checked on the PAL test disc: the player pauses on its stop
  code as on NTSC.
- 2026-11-11: the black level and CX modes remembered per disc with the other disc settings.
- 2026-11-11: edge-directed interpolation of the moving parts on SD (`--no-edge-directed` for the
  vertical average).
- 2026-11-12: `--write` files carry the disc's start timecode and chapter marks, and an NTSC
  input's closed captions as a subtitle stream.
- 2026-11-12: `--write-audio analog,efm,ac3` writes the listed tracks as separate audio streams.
