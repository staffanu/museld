# PAL Laserdisc Playback — Assessment and Plan

Written 2026-09-02 from a survey of the NTSC path; line references are to the tree at that
date (the class and file names were changed to their post-rename forms on 2026-10-02 --
`Ntsc…` became `Sdtv…` for everything both standards share -- so a reference like
`SdtvFrameReader.cpp:504` means the NTSC-only file of that date).  Reviewed 2026-09-30 (corrections folded in below) and work started 2026-10-01 on the
`pal-playback` branch.

## Status (2026-10-01)

Done, in one step rather than the phasing below:

- `sdtv/VideoStandard.{h,cpp}`: the runtime descriptor the SD pipeline reads its geometry,
  frame rate, RF carrier, VBI lines and noise windows from (`VideoStandard::ntsc()` /
  `pal()`); `SdtvInputBlock`, `SdtvRfDemodulator`, `SdtvFrameReader`, `SdtvFrame`,
  `SdtvShaders`, `SdtvDecoder` and `VbiData` take it.  The SD shaders are compiled twice,
  `ntsc_*.spv` and `pal_*.spv` (`-DPAL_GEOMETRY`, `shaders/muse/muse.h`); the literal
  NTSC geometry in their bodies became macros.
- `--input-type pal-rf`: 1135 × 625 line-locked frame buffer at 17.734375 MHz, 944 × 576
  image, sync and vertical anchoring (PAL's five broad pulses put both ends of the group on
  the same half-line phase, unlike NTSC's six), Philips code on lines 16-18 / 329-331,
  EFM audio, `--write` with BT.470BG tags, real-time pacing by the clock (the display loop
  otherwise runs at the 60 Hz refresh).  NTSC output is bit-identical to before.
- Monochrome only: the PAL shader build takes the luma through a [1 2 2 2 1]/8
  subcarrier-nulling filter (`sdtv_decode_single_field.comp` under `SDTV_PAL`); the comb,
  burst phase and motion shaders run but their colour output is discarded.
- Verified on the NYCSTM captures (D515 and LD-V4400 players, CLV, EFM): sync supported on
  610 of 625 lines (the 15 vertical-interval lines), VBI chapter/time/picture decode, both
  fields placed right.  The LD-V4400 captures run 0.7 % fast and lock anyway.
- Debug aid: `MUSELD_DUMP_FRAME=<path>` writes one frame buffer (and `<path>.raw`, the
  reader's line-locked composite) as float32.
- **Probe** (2026-10-04): PAL is detected.  Not by the field-lag autocorrelation proposed
  below -- wow moves the sync pulses by more than their width over a field, so nothing
  lines up at any one lag and the scores came out within ±0.05 of zero on real captures --
  but by the fallback: count the lines between vertical sync groups (runs of line periods
  spent mostly at sync tip) on a separate chunk of 3.5 PAL fields.  Content-independent and
  wow-immune; 312.3-312.5 on the four PAL captures, 261.3-262.7 on four NTSC ones, the
  audio carrier breaks ties when no group is found.  `--probe` prints the count.

Open after this step:
- **RF band** (resolved 2026-10-05): the "fc - 3.8 MHz component" is the **PAL pilot burst**,
  IEC 60856 §9.1.2 (`../analogue-video-specifications/docs/laserdisc/`): 240 × fH =
  3.75 MHz superimposed on the sync tip, 6/7 of blanking-to-white peak-to-peak (≈60 IRE),
  13.5 cycles from 0.5 to 4.1 µs after the sync edge (optionally in the equalizing and field
  pulses too); it modulates the tip carrier, hence the sideband at 6.76 − 3.75 = 3.0 MHz with a
  weak mirror at 10.5.  NTSC has none (its §9.1.2 is the colour burst).  ld-decode uses it for
  fine hsync timing (`pilot_mhz`).  What followed: the conceal shader's sync clamp now covers
  the PAL pulse (`SDTV_SYNC_END`), the sync-tip noise figure is "n/a" on PAL (the pilot fills
  the tip up to the rising edge once filtered), and the band stays 3.5-13.5 MHz for now.  The
  experiment that argued against the lower chroma sideband used sharp FIRs (0.6-1.0 MHz
  transitions, 160-270 taps = 4-7 µs at 40 MHz), whose ringing carried the pilot from the tip
  into the back porch (the "6-7 IRE of blanking noise"); ld-decode keeps the sideband on
  purpose with a *gentle* 2nd-order edge at 2.3 MHz (`FilterParams_PAL`, "to protect the lower
  chroma sideband and its group delay"), which rings for well under a microsecond.  So for the
  colour work: try a gentle low edge (2.3 MHz, ~2.5 MHz transition, ~65 taps) and judge it on
  decoded chroma noise against today's single-sideband band (half chroma amplitude, restored by
  the burst-referenced gain, -3 dB chroma SNR, phase intact).  decode-orc does not demodulate
  video RF (its PAL sinks are the ld-decode-tools ports on TBC'd baseband), so the choice is
  ld-decode's alone.  The pilot is a usable timebase reference for the PAL version of
  `tools/ntsc-burst-check.py`.
- Analog audio: the PAL carriers are wired in (683.6 / 1066.4 kHz, deviation assumed
  100 kHz) but untested -- no analog-audio PAL capture yet.  No CAV capture either.
- Colour (phase 4), black level (PAL has none: fixed at blanking), the Rec. 567 weighting
  and the combine shader's colorimetry (still SMPTE C / 2.2) for PAL.

## Summary

Adding PAL playback is a medium-to-large job. About 60 % of the pipeline is
standard-agnostic and ports for free, the RF/audio/dispatch plumbing is a few days of
parameterization, the sync and VBI line tables are a few more days of mechanical work, and
the PAL colour decoder is a real project: a rewrite of two shaders rather than a
parameterization. Rough budget: three to four weeks of focused work, given a decent PAL
RF capture to test against. Monochrome PAL playback with EFM/analog audio is a natural
first milestone (about one week) that proves everything except colour.

PAL laserdisc parameters that matter here (IEC 60857):

| Parameter | NTSC LD | PAL LD |
|---|---|---|
| Lines / fields per second | 525 / 59.94 | 625 / 50 |
| Line rate | 15734.27 Hz | 15625 Hz (0.7 % lower) |
| Video FM carrier, sync tip → white | 7.6 → 9.3 MHz | 6.76 → 7.9 MHz |
| Colour subcarrier | 3.579545 MHz | 4.433619 MHz (+25 Hz offset, 283.7516 cycles/line) |
| 4 fsc sampling grid | 910 samples/line, integer | 1135 + 4/625 samples/line, non-integer |
| Analog audio carriers | 2.3011 / 2.8125 MHz, ±100 kHz | 0.6836 / 1.0664 MHz, ±50 kHz |
| Digital audio | EFM (and AC3-RF at 2.88 MHz) | EFM **instead of** analog audio; no AC3 |
| Philips code | lines 16–18, biphase 2 MHz | same lines, same coding; second field at +312/313 |
| Closed captions | EIA-608 on line 21 | none (Teletext/WST on lines 7–22 if anything) |
| Colorimetry | SMPTE C, SMPTE 170M | EBU / BT.470BG |

Note the audio row: PAL discs carry **either** analog FM audio **or** EFM, never both, so
an audio carrier cannot serve as a PAL detector.

## What ports unchanged

- `efm/`, `rs/`, `bch/`, `filter/`, `efm/concealment/`: no video-standard dependency.
  EFM is CD-locked and identical on PAL discs.
- `ac3/`: unused on PAL; gate `--ac3` off for PAL input as `--cc-write` already is
  (`museld.cpp:1515-1516`).
- `musevk/`, `FrameBlitter`, `FrameExporter`, `OsdOverlay`, `TextRenderer`,
  `AudioPlayback`, `subtitles/` decoders, `input/` readers.
- `RfDemodulator` / `fm_quadrature.comp`: centre frequency and deviation are push
  constants.
- `VideoFileWriter`: fully rational-fps parameterized; needs only a new
  `VideoColorStandard::eBt470bg` case (`VideoFileWriter.cpp:328-342`). Audio sample rates
  are per `AudioMode`, not derived from the video clock.
- `Decoder::SourceDimensions` (`Decoder.h:30-35`) already abstracts geometry for the
  consumers.

## What needs parameterizing

- **`analog/AnalogAudioDemodulator`**: carrier literals at `.h:88-89` (2.3011/2.8125 MHz
  → 683.6 kHz/1.0664 MHz, deviation 100 → 50 kHz); channel-select lowpass at
  `.cpp:123-124` re-derived for 382 kHz carrier spacing instead of 511 kHz; the ≥7 MHz
  pre-mix floor (`.cpp:22-28`) still works but is wasteful for 1 MHz carriers. 75 µs
  de-emphasis is the same. CX is standard-agnostic; the CX enable comes from the Philips
  code and stays so. Output level `c_output_level` (`.h:113`) was calibrated on NTSC and
  should be re-measured.
- **`sdtv/SdtvRfDemodulator`**: `c_center_frequency` 8.5 → ~7.33 MHz, deviation 0.85 →
  0.57 MHz (`.h:97-98`); bandpass/lowpass edges (`.cpp:72, 89, 101`); dropout detector
  literals `30000/1001 * 525` and the 40 MHz slew reference (`.cpp:397-398`);
  `numberOfBlockBuffers()` divides by 30 (`.h:88`).
- **`museld.cpp`** (about six sites): window size from `SDTV_Y_BUF_WIDTH × 2·FIELD_HEIGHT`
  (:805-816); colour standard / DAR / fps (:858-869); `fields_per_second` pacing (:934);
  `InputType` enum, `--input-type` parsing, probe mapping and reader construction
  (:956-1103, :1418-1564).
- **Geometry constants** are duplicated: `ntsc/NtscConstants.h` (4 defines) and
  `shaders/muse/muse.h:22-27` (same 4 plus `SDTV_FIELD_START_X/Y`), with 129 hardcoded
  again in `SdtvDecoder.cpp:594`. Consolidate before adding a second standard.
- **`InputProbe`**: see the dedicated section below.

## What needs new code (mechanical)

- **Sync detection** (`SdtvFrameReader.cpp:504-533`, lock logic at :326, :377, :385):
  the vertical-sync half-line table hardcodes NTSC line numbers 1–9 / 263–272. PAL needs
  625/312.5 with its 2.5-line broad-pulse sequence and 2.5-line equalizing runs.
- **VBI** (`SdtvFrame.cpp`): white-flag / blank rows (:69, :130), Philips code lines
  `{16,17,18,278,279,280,281}` (:206) → second field at +312/313; CC line 21 (:190)
  becomes a no-op (`cc_bytes` stays `nullopt`); burst columns 78..110 and porch windows
  (:50-52, :98) move to the 1135 grid. The IEC 60857 code patterns themselves (:237-300)
  are identical on PAL.
- **`VbiData.cpp:57`**: `ntsc_fps` for CLV picture → time becomes 25.
- **Frame buffer**: `SdtvInputBlock.h:26-28` (910 × 525 @ 14.318 MHz) → 1135 × 625 @
  17.734 MHz. Decide how to handle the +4 samples per frame from the 25 Hz offset (see
  colour, below).
- **`NtscCadenceTracker`**: 3:2 pulldown only; inert on PAL, or grows 2:2 detection.
  Its 8-sample fsc-nulling box (`.cpp:75`) assumes the 4 fsc grid, as does
  `sdtv_detect_motion.comp:22-23`.
- **SNR reporting** in `SdtvDecoder.cpp:230-264` hardcodes the System M sample rate,
  4.2 MHz bandwidth and Rec.567 weighting; PAL is 5.0/5.5 MHz.

## The hard part: colour

Every stage of the chroma chain assumes NTSC subcarrier topology on a line-locked
4 fsc grid:

- `sdtv_detect_color_burst_phase.comp:19-27`: `col % 4` sine/cosine LUT demodulator,
  burst window at fixed columns, 20 IRE burst amplitude calibration.
- `sdtv_decode_single_field.comp:99-113`: 3-line spatial comb assuming adjacent lines
  are 180° apart in subcarrier phase.
- `sdtv_decode_single_field.comp:118-120, 350-353` and `SdtvDecoder.cpp:580-583`:
  temporal (3D) comb assuming the subcarrier inverts frame to frame.
- `sdtv_combine_still_and_moving.comp:117-140`: SMPTE C primaries → sRGB and the
  NTSC CRT EOTF.
- `sdtv_deemphasis.comp`: LaserVision de-emphasis FIR precomputed for the
  4 × 3.58 MHz grid (same time constants on PAL, taps must be regenerated at 4 × 4.43 MHz).
- Chroma rotation `185.8° + tint` in `SdtvDecoder.cpp:72-74` is calibrated for I/Q.

PAL breaks all of this: V-axis switching per line, the ±45° swinging burst, the 4-field
(8-field with the 25 Hz offset) sequence, and the non-integer 1135.0064 samples per line
mean a line-locked grid gives no clean `% 4` phase LUT. The one piece that ports is the
burst-referenced U/V demodulation at `sdtv_decode_single_field.comp:415-421`.

Sampling grid: **line-locked 1135 samples per line, with per-line burst phase.**
Exact 4 fsc is 1135.0064 samples per line (integer only per frame: 709379), so no grid
gives an integer line *and* a fixed column-to-subcarrier phase. Subcarrier-locked sampling
keeps the `col % 4` LUT exact but drifts hsync by 4 samples per frame (a top-to-bottom
shear, and a moving frame start). Line-locked 1135 (17.734375 MHz, 100 Hz below 4 fsc)
keeps the DPLL on hsync and the frame buffer rectangular; the subcarrier then walks
0.576° per line relative to the grid, exactly one cycle per frame (the 25 Hz offset).
The burst phase is already measured per line for NTSC and applied as a rotation, so
making that the phase reference, with the LUT only as a basis, absorbs the drift; the
V switch and swinging burst require per-line phase handling anyway. Same choice as
ld-decode's PAL path.

Comb structure (corrected 2026-09-30): adjacent lines of a field are one line period
apart, 283.75 cycles = 270.6° of subcarrier, so NTSC's 3-line comb (adjacent lines 180°
apart) does not carry over. Lines n ± 2 of a field are 2270.0128 samples apart (180° +
1.15°) and share the V-switch sign, which is the 2H comb; the simpler first step is the
classic 1H delay-line U/V separation. Temporally the subcarrier advances 270° per frame and
the V switch flips (625 is odd), so frames N ± 1 do not cancel: a PAL 3D comb needs frames
N ± 2, i.e. more frame history than the current three-frame window -- a separate, later
phase.

Expect: a new burst-phase shader, a new PAL field decoder (roughly 500 lines of GLSL),
EBU primaries and BT.470 gamma in the combine shader, and a re-derived motion detector.
This is the part that needs real captures — phase conventions must be verified against
picture, not spec.

## InputProbe: detecting PAL

Current model (`InputProbe.cpp:33-41, 545-573`): the analytic signal's instantaneous
frequency is block-averaged by 16 and autocorrelated over lags 480..6400 samples to find
the line period; classification is by video carrier cycles per line (NTSC 470–640, MUSE
260–430) with the NTSC analog audio carrier ratio as the decisive tie-breaker.

Neither discriminator works for PAL:

- Line rate differs by 0.7 %, far inside the autocorrelation peak's resolution.
- Carrier band is ~433–506 cycles per line, overlapping NTSC's window and touching
  MUSE's.
- Audio carrier is absent on digital-sound PAL discs.

**Proposed: autocorrelate at the field lag.** The averaged instantaneous frequency is
already baseband video. Once the line period L is known, evaluate the normalized
autocorrelation in two small windows around 262.5 L and 312.5 L. Both lags are
half-integer lines, so horizontal sync pulses either align exactly (true field period,
strong positive peak) or fall half a line off with sync tip on active video (negative).
Strongly signed, threshold-free, no sync-level calibration, tolerant of dropouts, and
only two short lag scans instead of a full sweep. Add the frame lags 525 L / 625 L as
confirmation. MUSE peaks at neither, which doubles as a sanity check on the carrier band.

Fallback / extension: count samples at the minimum frequency level per line period.
Normal lines spend ~7 % at sync tip, vertical-serration lines ~90 %; runs of such lines
mark the vertical intervals and their spacing in lines is 262.5 or 312.5 directly. Needs a
threshold but also yields the exact field start.

**Chunk length must scale.** `c_chunk_samples = 2^21` covers:

| Capture rate | Chunk duration | PAL fields per chunk |
|---|---|---|
| 40 MHz | 52 ms | 2.6 |
| 62.5 MHz | 34 ms | 1.7 |
| 100 MHz | 21 ms | 1.05 |

The field-lag test needs at least two field periods, preferably three or four. Since L is
measured first, read a second chunk sized from it (≈ 4 × 312.5 × L samples) rather than
enlarging every chunk. `analyzeFile`'s chunk-offset and median logic is unaffected.

Classifier changes: the carrier band still excludes MUSE and catches harmonic locks; the
field period becomes the NTSC/PAL decision; `estimateSampleFrequency` substitutes
15625 Hz for PAL and snaps against `c_known_rates` unchanged. Add `ePalRf` to
`InputProbeResult::Type` (`InputProbe.h:30`) and `InputType` (`museld.cpp:956`), plus
the `--input-type pal-rf` spelling. Extend `InputProbeTest` with a PAL fixture.

Estimated size: ~100 lines plus the variable-length read.

## Suggested phasing

1. **Refactor, no behaviour change.** Consolidate geometry constants into one header
   shared with the shaders; introduce a video-standard descriptor (lines, field lines,
   samples per line, sampling frequency, line rate, fps, carrier centre/deviation, VBI
   line numbers) and thread it through `SdtvInputBlock`, `SdtvFrameReader`, `SdtvFrame`,
   `SdtvDecoder`, `SdtvRfDemodulator`. Decide between runtime struct and template
   parameter; the existing `if constexpr` dispatch on block type in `museld.cpp` suggests
   a `PalInputBlock` alias over a templated block is the path of least resistance.
2. **Probe.** Field-lag detection and `ePalRf`, testable on captures before anything
   else exists.
3. **Monochrome PAL.** RF demod parameters, sync tables, 1135 × 625 buffer, VBI lines,
   luma-only display at 50 fps, 4:3, BT.470BG tagging in the writer. Analog audio carriers
   and EFM. Verify Philips codes (chapter, picture number, CLV time) decode.
4. **Colour.** Burst-phase shader, PAL field decoder, combine shader colorimetry,
   de-emphasis taps, motion detector. Tint control semantics for PAL (hue errors cancel;
   probably disable).
5. **Polish.** Cadence tracker behaviour on PAL, SNR reporting, OCR band geometry,
   docs (`docs/museld.md`), CI test fixture.

## Open questions

- **Test material.** Which PAL RF captures are available, at what rates, and do they cover
  analog-audio, digital-audio, CAV and CLV discs? Colour work is blocked without at least
  one clean capture.
- Whether Teletext subtitle decoding is ever wanted; the `cc_bytes` path could carry it
  but it is out of scope here.
- Whether `--ocr` band geometry assumes 480 active lines anywhere.
- PAL discs with CX: rare, but the Philips-code CX flag should still be honoured.
