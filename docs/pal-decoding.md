# PAL Laserdisc Decoding

How museld's `--input-type pal-rf` path works, what it was calibrated against, and what
differs from NTSC.  This replaces the September 2026 plan (`pal-playback-plan.md`); the
work it planned was done on the `pal-playback` branch in October 2026 and is on master.
The IEC 60856 (PAL) and 60857 (NTSC) laserdisc specifications are OCR'd in
`../analogue-video-specifications/docs/laserdisc/`; check there first for standards
questions.

## Parameters

| Parameter | NTSC LD | PAL LD |
|---|---|---|
| Lines / fields per second | 525 / 59.94 | 625 / 50 |
| Line rate | 15734.27 Hz | 15625 Hz (0.7 % lower) |
| Video FM carrier, sync tip → white | 7.6 → 9.3 MHz | 6.76 → 7.9 MHz |
| Pilot burst on the sync tip | none | 240 fH = 3.75 MHz, 0.5-4.1 µs after the edge, ≈60 IRE p-p |
| Colour subcarrier | 3.579545 MHz | 4.433619 MHz (283.7516 cycles/line, 25 Hz offset) |
| 4 fsc sampling grid | 910 samples/line, integer | 1135.0064 samples/line |
| Frame buffer | 910 × 525 at 14.318 MHz | 1135 × 625 at 17.734375 MHz (line locked) |
| Picture | 764 × 480 | 944 × 576 |
| Analog audio carriers | 2.3011 / 2.8125 MHz, ±100 kHz | 683.6 / 1066.4 kHz (43.75 / 68.25 fH), ±100 kHz |
| Digital audio | EFM (and AC3-RF at 2.88 MHz) | EFM **instead of** analog audio; no AC3 |
| Philips code | lines 16-18 and 278-280 | lines 16-18 and 329-331, same coding |
| Closed captions | EIA-608 on line 21 | none |
| Black level | 7.5 IRE setup (NTSC-M) or 0 (NTSC-J) | at blanking |
| Colorimetry | SMPTE C, SMPTE 170M | EBU, BT.470BG |

PAL discs carry either analog FM audio or EFM, never both, so an audio carrier cannot
serve as a PAL detector.  The RF chroma reaches the demodulator through the **lower**
sideband only: the upper one, at 11.5 MHz, is lost to the disc MTF.

## The standard descriptor

`sdtv/VideoStandard.{h,cpp}` (`VideoStandard::ntsc()` / `pal()`) holds everything the SD
pipeline reads per standard: frame geometry, frame rate, the RF carrier and band edges,
the pilot frequency, the analog audio carriers and channel filter edges, the VBI code
lines, the noise measurement windows, and the feature flags (`ntsc_chroma`,
`has_black_setup`, `has_closed_captions`, `has_white_flag`, `has_film_cadence`,
`rescue_floor`).  `SdtvInputBlock`, `SdtvRfDemodulator`, `SdtvFrameReader`, `SdtvFrame`,
`SdtvShaders`, `SdtvDecoder` and `VbiData` take it.  The `shaders/sdtv/sdtv_*.comp` sources
are compiled once per standard, as `ntsc_*.spv` and as `pal_*.spv` with `-DSDTV_PAL`; the
`SDTV_*` geometry macros in `shaders/muse/muse.h` take the PAL values there.  Naming:
`Sdtv…` is what both standards share, `Ntsc…`/`Pal…` only what belongs to one of them
(`NtscCadenceTracker` for 3:2, `PalCadenceTracker` for 2:2; `VideoStandard::film_pulldown`
picks the tracker).

Film mode exists on both standards but looks for a different cadence.  NTSC film is 3:2,
so the `NtscCadenceTracker` finds the repeated field.  PAL film is 2:2, one film frame per
video frame with no repeated field, so the `PalCadenceTracker` instead measures how well
each field weaves with both of its neighbours and votes per frame on which pairing is the
film frame: the frame's own two fields (`F1+F2`) or the previous frame's second field with
this frame's first (`F2+F1`).  Four moving frames that agree lock it, stills neither lock
nor unlock, video-sourced material combs in both pairings and never locks, and two
contradicting moving frames in a row unlock it.  Locked, the field that pairs within the
frame weaves and the other holds, as on NTSC.  The OSD shows `Telecine: 2:2 locked` with
the pairing while paused.  Locks on Empire D515 at frame 10, LD-V4400 at 49, ESB at 433,
Pioneer94 at 142; never on The Big Day, which is video.

Things that are NTSC-only and simply gated off on PAL: the automatic NTSC-M/J black
level, the white flag, line-21 closed captions and `--cc-write`, AC3-RF.  `--tint` and
`--saturation` apply to both.

## RF demodulation

The band kept is 2.3-14 MHz with a gentle 2.5 MHz transition: the lower chroma sideband
sits at 6.76 − 4.43 = 2.3-3.5 MHz, just above the EFM band, and is not optional.  With
NTSC's 3.5 MHz edge the burst demodulated at 0.03 (the noise floor) against 0.25 with the
sideband admitted.  Sharp FIRs (0.6-1.0 MHz transitions, 160-270 taps) are wrong here even
though they pass the sideband: their ringing carries the pilot burst into the back porch.
ld-decode's second-order edge at 2.3 MHz ("to protect the lower chroma sideband and its
group delay") is the same idea.

The **pilot burst** modulates the sync-tip carrier, hence the sideband at 6.76 − 3.75 =
3.0 MHz in the RF spectrum with a weak mirror at 10.5 (it was first taken for an unknown
"fc − 3.8 MHz component").  Consequences: the conceal shader's sync clamp covers the pulse
(`SDTV_SYNC_END`), the sync-tip noise figure is "n/a" on PAL (the pilot fills the tip up to
the rising edge once filtered), and the probe removes it with a 2 µs running mean before
slicing.  The dropout detector's literals are per standard; the de-emphasis time constants
are the same as NTSC's and its 24 taps are regenerated on the PAL grid at build time.

## Sync and timebase

The feed-forward timebase of `SdtvFrameReader` (sync pass, line lattice, fixed-lag Kalman
smoother, per-line resample) is shared.  PAL differences:

- **Vertical anchoring.**  PAL's five broad pulses put both ends of the group on the same
  half-line phase, unlike NTSC's six, so the anchor test that rejected a complete group and
  anchored on partial ones swapped the fields.  `vsync_broad_start_line` /
  `vsync_broad_pulses` describe the group per standard.
- **Pilot refinement** (`SdtvFrameReader::pilotCorrection`).  The curve's ~1.4 ns
  line-to-line timing error cannot be cancelled by the colour burst for the picture further
  along the line, and showed as per-line jitter of the chroma reference (1.8° rms of phase,
  4.5 % of magnitude, proportional to saturation: horizontal streaking on flat colour).  The
  reader measures the pilot phase 0.8-3.8 µs after each curve line start against a slowly
  tracked reference and shifts the start by the deviation: 0.4 ns residual jitter, 5 ns
  maximum error.
- **Fast captures.**  LD-V4400 captures run 0.7 % fast (63.55 µs lines) and lock without a
  period hint.

Check it with `tools/pal-pilot-check.py <prefix> <dump> <out> <fs>` on a
`MUSELD_DUMP_TIMEBASE` + `MUSELD_DUMP_DEMOD` pair from the same run: the curve against the
pilot is ±15-30 ns with 1.6-2.2 ns line-to-line jitter on the GGV1011 and the NYCSTM
captures (the same as NTSC's burst check); on the `<prefix>.pilot.f64` corrected curve the
residual is 0.4 ns.  Over 20 s and 984 fields of the GGV1011: p1/p99 −13/+37 ns, max 53 ns.
Anything in the sync pass that is a count of samples must be derived from a time, never a
constant (see the 30 MHz note in `CLAUDE.md`).

## VBI

The Philips code is on lines 16-18 of each field (329-331 in the frame for field 2),
probed with a one-line margin.  The code patterns (chapter, CLV time, CAV picture number,
picture stop, CX flag, lead-in/out) are IEC 60857's and decode unchanged; CLV time uses
25 frames/s.  The X1 chapter-stop and no-picture-stop flag bits are masked (they are set
over the first 400 tracks of a chapter, and on every picture of some discs).  The GGV1011
CAV picture numbers decode and count (20794, 20795, … in chapter 34).  Teletext is not
decoded and not planned.

## Colour

The sampling grid is line-locked at 1135 samples per line (17.734375 MHz, 100 Hz below
4 fsc), so the frame buffer stays rectangular and the timebase on hsync; the subcarrier
then walks 0.576° per line relative to the grid, one cycle per frame.  Every line decodes
against its own burst, which absorbs the walk, the V switch and the swinging burst.  In the
grid frame the burst sequence is b, b, −b, −b: the structural 270.576° advance per line and
the ±90° swing are degenerate on a 4 fsc grid, which is what the line-pair derivation
below untangles.

**2D comb** (`sdtv_decode_single_field.comp` under `SDTV_PAL`, helpers in
`sdtv_comb_taps.h`).  Adjacent lines of a field are 283.75 cycles apart, so their chroma
sits at ∓90° from this line's and cancels in their sum whatever the V switch does (they
share its sign): chroma = f − ½(f₋₁ + f₊₁), with a 1 % luma leak, the NTSC 3-line comb with
the centre tap doubled; the field's last line falls back to a two-line comb with the line
two above (181°, same V sign).  Luma is the composite less the chroma band of that
estimate.  Demodulation is a 19-tap window with a lowpass of its own on PAL: a 0.9 MHz
design on the 17.7 MHz grid (−3.5 dB at 0.9 MHz, −11 at 1.5, −22 at 2.0, within half a dB of
the centre line of ld-decode's PAL decoder's raised-cosine window), where NTSC's 1.0 MHz taps
pass 1.24 MHz.  On its own the narrower lowpass gained nothing; with the vertical filter
below in place it took the Pioneer 94 edge fringes down a further tenth.

**The −U axis** comes from a line pair: the neighbour's burst de-rotated by the structural
270.576° added to the line's own.  The ±45° swings cancel, leaving 2 cos 45° of the burst
amplitude along −U.  `pal_smoothed_axis` averages the pair estimates over five lines
(weights 1 2 3 2 1, each brought into the line's frame by the structural rotation) and
scales the result to nine estimates' worth whatever the window held, so the field's first
lines and the neighbours of a lost burst get the same gain as the rest (before the scaling
the first two and last three lines of each field decoded oversaturated).  The smoothing removes
the burst's measurement noise, about 2° per pair on a 27 dB capture: bar phase jitter
1.82° → 1.23°, magnitude 4.5 % → 3.1 %.  A nine-line window gained nothing more.

**The V switch** of a line is the side of the −U axis its burst swung to.  It is voted over
the same five lines, each neighbour's burst read against the axis rotated into that line's
frame and its side flipped by the line parity (the switch alternates every line), so a line
whose own burst is lost (a dropout over the 2.25 µs burst window) still decodes, and one
noisy burst cannot invert a line's hue.  Zeroing every seventh burst in the detector
changes the GGV1011 bars by at most 15 levels on 37 pixels.  `MUSELD_PAL_VFLIP=1` inverts
the switch, a calibration aid.

**Vertical chroma filter** (the PAL delay-line step, widened).  With the temporal comb on
(key 4; this is the key's only meaning on PAL) a line's U and V are averaged with the three
lines each side of it in the field, weights 1 : 0.835 : 0.447 : 0.095 for distances 0 to 3,
the vertical taps of the 2D chroma filter of ld-decode's PAL decoder (William Andrew Steer's)
at the centre of its raised-cosine window.  Each line's U and V come from its own column
taps through the same spatial/temporal selector and its own burst reference (the single
field shader runs twice: a first dispatch writes every column's own-line estimate to
`pal_uv`, the decode reads its neighbours back), lines flagged as dropouts at the column
are left out, and the field's first and last lines use what is there.  It keeps what the
line-pair average gave -- 3 dB less chroma noise, a differential phase error landing on V
with opposite signs on consecutive lines and cancelling instead of showing as Hanover bars
(R−G noise on the GGV1011 modulated staircase 10.3 → 6.9, line alternation on the NYCSTM
1.05 → 0.41 with the pair; the sculpture in Empire D515 at 24:01 that the pair took from 4.5
to 1.7 of line-to-line chroma alternation) -- and goes after the 3-line comb's one real
weakness: wherever the picture differs from line to line the comb leaks luma into the
chroma, and on a diagonal high-contrast edge the leak's phase rotates with the subcarrier
from line to line while the chroma holds, which decoded as rainbow fringes along the edges
of clothing on the Pioneer 94 clip (pictures 40970-41000).  The average thins the rotating
leak and leaves the chroma: local chroma variation in two edge regions of picture 40986
4.61 → 2.46 and 6.93 → 3.48 (2.70 and 3.97 before the PAL lowpass above), the GGV1011 bars'
colours unchanged to within a level, the Big Day frame's chroma noise 5.9 → 3.1, and the
NTSC path byte-identical.  ld-decode's 2D decoder measures 3.04 and 4.46 on the same
regions and its Transform decoder 2.61 and 3.54, with a luma 9 % softer; what is left of
the fringes in museld is visible at full zoom and would need the 2D filter's narrower
horizontal windows on the outer lines, or more, to go further.  The price is vertical
chroma resolution, which PAL decoders have always paid; the frame time is unchanged.

**Temporal (3D) comb.**  The subcarrier advances 270° per frame and the V switch flips (625
is odd), so frames N ± 1 do not cancel; N ± 2 do (chroma inverted, V switch matching), and
NTSC's phase assumptions hold at that spacing.  The decoder keeps 3d + 1 frames with d = 2
on PAL (1 on NTSC) and shows frame N two reads behind, the audio held one read longer to
match; `sdtv_chroma_taps.comp` and the motion detector are reused with only the spatial
comb swapped, the frame-pair rotation taken from the smoothed axes rather than two single
bursts.  All seven slots are live: the frames the comb skips on one read are the ones it
uses on the next, two interleaved chains.  Luma and chroma noise −3 dB on the bars on top
of the line averaging, no ghosting on moving content, 7.5 ms of the 40 ms budget.

**Weave masks.**  The spacing-2 motion masks hide frame-to-frame motion from the
de-interlacer (the GGV1011's line pattern inverts every frame and is identical two frames
apart, so the combine wove each field with the inverted one).  On PAL the motion detector
runs a second time on the consecutive frames into its own mask set for the combine, with
the two-frame differences sent through the fsc-nulling box as well (`box_aligned`, no PAL
frame pair at spacing 1 is phase aligned); the comb and the dropout paths keep the
spacing-2 masks.

**Calibration** (GGV1011 colour bars, measured on the lossless `--write` output): hue
errors −0.6…+0.3° with the demodulation angle at the structural 180° (NTSC's is calibrated
at 185.8°), so there is no tint offset; saturation within ±10 % of what each bar's own luma
implies after the burst AGC constant went from the nominal 4.85 to 3.75 (bright bars read
slightly high, dark ones low: differential gain in the signal, not a constant); full-white
field Y = 0.996, black bar 0.012.  Even/odd row chroma difference 0.0000 on full colour
fields (no Hanover bars), 1-2 % uniformity over the frame, crosshatch cells square to 0.2 %
after the 944/1135 pixel aspect with 0.006 of cross-colour.  The red and green full fields
read 4-6° off the bar hues identically on the spatial and temporal paths; they are not the
bar primaries (the blue field's chroma is 0.43 against a 75 % blue's 0.34), so the bars
remain the reference.  The zone plate is animated, so its residual centre chroma is
moving-content cross-colour.  The combine shader has the EBU primaries; PAL has no setup,
black is at blanking.

## Dropouts

The dropout paths are NTSC's.  The illegal-level rescue substitutes the nearest healthy
line of the field (any line will do on PAL: each decodes against its own burst), and a
flagged pixel takes the same pixel of the stiller neighbour frame decoded in place, the
frame source selected per access since GLSL has no buffer references.
`VideoStandard::rescue_floor` is 0.08 on PAL (NTSC 0.02): the filtered luma beside a
one-sample white line legally undershoots to −0.06, and with the noise-scaled floor at
−0.05 the GGV1011 crosshatch had its vertical lines repainted for ~8 frames after each
cut, the motion masks' persistence supplying the "moving" condition.  On the NYCSTM
(27 dB) the paths remove the specks and touch 200-400 pixels a frame; on a rotted disc
(13 dB) the motion masks are noise and the donor misplaces content, so `--no-dropout` is
the right setting there.  `MUSELD_DUMP_FRAME` writes the detector's flags to `<path>.do`,
which separates the detector from the rescue.

## Audio

EFM is CD-locked and identical to NTSC's.  Analog audio (IEC 60856 clause 8): channel I
(left) at 43.75 fH = 683.6 kHz, channel II (right) at 68.25 fH = 1066.4 kHz, ±100 kHz for
100 % modulation (the test disc's 1 kHz tone spreads exactly ±100 kHz in the RF and
decodes at −12.9 dBFS where the 75 µs de-emphasis predicts −12.6).  The carriers are only
383 kHz apart, so the channel-select filter is per standard (PAL pass 150 / stop 240 kHz;
NTSC 315 / 330 for its 511 kHz spacing): with the NTSC edges the right carrier's inner
sideband beat the left channel's discriminator into permanent squelch.  CX follows the
Philips code flag as on NTSC.

## Probe

PAL is told from NTSC by the lines between vertical sync groups (runs of line periods
spent mostly at sync tip) on a separate chunk of 3.5 PAL fields: 312.3-312.5 on PAL
captures, 261.3-262.7 on NTSC ones; the audio carrier breaks ties when no group is found.
Field-lag autocorrelation did not work: wow moves the sync pulses by more than their width
over a field, so nothing lines up at any one lag.  The slicing level is set from the
tip-to-white span (blanking is 0.3 of it above the tip on both standards), not a
percentile that bright content moves to blanking, and the pilot is removed by a 2 µs
running mean first.  The carrier band (433-506 cycles per line) overlaps NTSC's and the
line rate is 0.7 % off, so neither discriminates.  `--probe` prints the count;
`InputProbeTest` has a PAL fixture.

## Noise, PAL against NTSC

The GGV1069 is the NTSC twin of the GGV1011 (same pattern sequence).  Capture SNR 29.0 dB
(PAL) vs 31.7 dB (NTSC); in the decoded bars the random noise is 2.4× larger on PAL in Y, U
and V (7.5 dB).  All of it is the signal, not the decoder: 2.7 dB of capture, plus for luma
the 5.8 vs 4.2 MHz bandwidth under FM's triangular noise ((5.8/4.2)³ = 4.2 dB → 6.9 dB
expected, 6 measured), and for chroma the single lower sideband (3 dB) and the subcarrier's
place in that spectrum ((4.43/3.58)² = 1.9 dB → 7.6 dB expected, 7.5 measured).  The SNR
report needs nothing PAL-specific: the Rec. 567 weighting is the unified network (BT.1439
Annex 2, the same for 525 and 625), and the bandwidth and de-emphasis terms follow the
standard.

## Test material

- **GGV1011** PAL CAV test disc, side 1 (DdD capture, FLAC in Ogg, 23.7 GB, ~980 s,
  `~/Downloads/GGV1011_CAV_PAL_side1_*.ogg`): line patterns (5-30 s), sweep, fields, colour
  bars at ~190-210 s, colour fields, window / crosshatch / dots / zone plate, CAV still
  photos from ~800 s.  Analog sound: left carrier unmodulated, right a 1 kHz tone.
- **GGV1069**, its NTSC twin (`~/Downloads`, `.ogg` = ldf): the like-for-like noise
  comparison, a real picture stop at picture 475.
- **NYCSTM** CLV captures with EFM audio (`~/Downloads/rotter-samples`, 40 MHz `.lds`,
  D515 and LD-V4400 players): a colour programme mixed with black-and-white archive film,
  27 dB on the good copy, 13 dB on the rotted one.  Sync supported on 610 of 625 lines (the
  15 vertical-interval lines), VBI chapter/time decode, both fields placed right.

## Still to check

- The GGV1011's CAV behaviour in the player: picture stops and the P key share NTSC's code
  but have not been exercised on a PAL disc.
- The saturation spread on the bars (bright high, dark low) is differential gain; nothing
  in the decoder to adjust, but a disc with a known-good staircase would settle it.
