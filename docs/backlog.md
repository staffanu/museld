# Developer backlog

Work worth doing, in rough order of value, with the reasoning behind each item so it can be
picked up cold.  This is a working list for development, not the list of known limitations in
`museld.md`: nothing here is promised, and items leave when done or when they stop making sense.
Last revised 2026-11-23.

## Housekeeping

- **Remove the abseil workaround in CI** once MSYS2 rebuilds `mingw-w64-ucrt-x86_64-onnxruntime`
  against abseil 20260817 (or later).  The step "Work around onnxruntime linked against an older
  abseil" in `.github/workflows/ci.yml` prints "this workaround step can be removed" when that
  has happened.  Background: MSYS2 moved abseil-cpp on 2026-10-29 without rebuilding onnxruntime
  1.29.0-2, and the full Windows package's museld.exe failed to load.
- **muse-16 (16.2 MHz baseband) input type** is legacy: keep it working for existing u16 files,
  no new formats or level work, remove when nothing needs it.
## Picture quality

- **MUSE impulse noise (FM clicks) concealment.**  White-on-black titles on a weak-carrier
  capture (a Top Gun side from an HLD-X9, 2026-11-19) show black breaks across the white strokes:
  one- or two-sample impulses of either polarity in the demodulated baseband, about 40 per frame
  in the picture (250 ppm of the samples), the negative ones mostly on white, where the disc MTF
  leaves the least carrier.  They are the demodulator's cycle slips, not dropouts: the carrier
  envelope collapses for a single sample (2% of the neighbouring lines' level) and is back
  within three, which the dropout detector's 9-sample box sees as 60-97%, and a narrower box
  cannot be used, since the emphasised signal's own edges drive the sidebands out of the band
  and take the single-sample envelope just as low at one position in a hundred, in every frame.
  What separates a click is that it is not repeatable: the same-phase frames before and after
  agree (they register to 0.04 of a sample) and the clicked one differs, and on a static stroke
  the lines above and below agree too.  So the place is after demodulation, in the decode: a
  sample that departs from both its same-phase temporal neighbours while they agree, or from
  the vertical neighbours of a still stroke, replaced by them; the intra path, which is where it
  is most visible, has only the vertical option.  The measurement recipe: `--write-muse16` of
  the stretch, each line against the same line two frames on either side, runs of two or more
  samples off by half the black-white span, and the shift of the line against those neighbours
  by cross-correlation (none here).  Other discs show the same distortion; to be judged against
  the Sony MUSE decoder's picture of them first.
- **Gate the SDTV sync measurements on RF dropouts.**  The sync pass finds pulses in the noise
  of a carrier collapse, and a short burst of them unsettles the lattice: a clean PAL capture
  with the RF replaced by 2 % noise for 1 ms twenty times a second (an experiment of
  2026-11-22, the burst copies made offline) made the reader re-anchor on a vertical drift of
  one to eight lines 35 times in 20 s, where the same capture undamaged does so never, and the
  curve's local line period was off by up to 9 % around the bursts.  The same mechanism kept
  the reader anchored through a no-disc gap (fixed 2026-11-21 by dropping the lattice on a
  frame mostly without pulses), but a burst is far shorter than a frame and the lattice should
  coast through it on its period instead of measuring in it: the dropout detector's envelope
  already marks the stretch, so a measurement inside a flagged run can be refused, and the
  period held, as the MUSE reader coasts its audio blocks.  Judge it by the re-anchor count and
  the curve against the undamaged capture's, and on a scratched disc by the regression's
  Abyss scratch clip.  Found while testing whether the video line clock could steer the EFM
  bit clock -- the two are genlocked to 0.001 ppm and follow each other through 2000 ppm of
  wow to 0.04 bits, but the EFM loop gained nothing from it on noise or rot, and through
  bursts the unsettled curve made it worse, so that idea is dropped.
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
## Functionality

- **Per-capture remembered settings:** a sidecar file next to the input, written on quit and
  read on open, for what the state file remembers (aspect, overscan, audio track, subtitle
  tracks, black level, CX, tint).
- **Player-like transport:** step backwards, slow motion, fast scan with picture.  These need
  frames decoded out of order or at another rate, unlike the searches (done 2026-11-14), which
  only seek.
- **AC3 bitstream passthrough** to the audio device for an AV receiver, instead of decoding to
  5.1 PCM.  Natural for the decoder-box setup.
- **Direct USB capture input** (Domesday Duplicator, Picoscope) without an external program.
- **OCR polish:** a presentation-delay buffer would remove the one-second subtitle lag and allow
  inpainting the burned-in text away; in-flight translations should be flushed at exit.

## Done since this list was started

- 2026-11-21: live type switching tried on the ethadc UDP stream, from the desk: `ethadc-tx`
  (the ethadc repository's stand-in for the hardware) streamed compound captures -- a disc,
  8 s of no-carrier noise, another disc, made by `tools/compound-capture.py` -- to
  `museld udp://`, NTSC to PAL and to NTSC at 40 MHz, NTSC to MUSE and MUSE to MUSE at
  62.5 MHz.  Found and fixed: the SDTV reader stayed anchored through the gap (the sync
  detector finds pulses in noise, so the no-pulses guard never fired), the picture showed
  decoded noise instead of NO SIGNAL, and the next disc of the same standard took more than
  ten seconds of drift corrections to settle; now a frame with most lines unsupported by a
  pulse reports no signal, the lattice is dropped after half a second of it, and the next disc
  is acquired within about a second.  The analog-to-EFM audio fallback no longer fires during
  the gap.  Measured: a switch of standard comes 3 s after the first classifiable chunk (three
  agreeing chunks a second apart), 5-6 s after a spinning-up disc appears since its first
  seconds classify as nothing or at a line rate off the stream's; the three-chunk count is
  kept, since noise never classified as a type and the count costs two seconds.  The MUSE
  reader was right already.  On a Mac the loopback loses a tenth of 1440-byte packets at
  40 MHz (kern.ipc.maxsockbuf caps the receive buffer at 8 MiB); `--jumbo` cures it.
- 2026-11-21: the settings the keys change carry to the next input of the run (a disc's other
  side, on the command line or chosen in the browser), and the disc settings -- aspect,
  overscan, audio track, subtitle tracks, black level, CX, tint -- are applied by how recent
  the intent is: over the defaults the disc's memory, then the options given on the command
  line (before, the memory silently undid an option), then what the keys changed on the
  previous input of the same standard.  The state file holds only what differs from the
  defaults, so an untouched disc has no entry and nothing freezes the defaults of the day.
  The tint became a live setting the same day (K and L, the menu's Tint line, remembered per
  disc).
- 2026-11-21: the PAL dropout detector threshold, measured and left alone.  The slew
  corroboration's threshold is in units of the standard's deviation and was calibrated on
  NTSC; on PAL (deviation 0.57 MHz against 0.85, video band 5.8 against 4.2 MHz) legal signal
  crosses it 40 times as often, and the RF detector flags 390-490 ppm of the samples on clean
  PAL discs against 5 on NTSC at equal video SNR.  A ground truth was built from three
  independent masterings of one PAL programme (NYCSTM: two good copies at 27 dB and the
  rotted one at 13 dB, 18 s aligned by disc code to the line, by cross-correlation of the
  derivative to the sub-sample, compared after a 2 MHz low-pass with an edge mask, since the
  masterings' edge responses differ, and each departure attributed by the RF envelope): the
  good copies hold three real departures in 18 s, so their flags are false to within rounding;
  the rotted copy's 126 departures are 116 cycle slips of the demodulation (one sample, 400
  IRE after the low-pass, the envelope holding) and two carrier collapses of a microsecond.
  Doubling the threshold (what the bandwidth-over-deviation ratio suggests) takes the false
  flags from 494 to 18 ppm on the good copy and 391 to 1 on Big Day and keeps both collapses,
  but the picture does not change: the pinned-level corroboration that follows already
  discards the false RF flags, and the painted pixels per frame stay at 164 to 159 on the good
  copy and 120 to 119 on Big Day.  So the threshold is miscalibrated on paper and harmless in
  the picture, and is left as it is rather than re-recording the PAL goldens for nothing.
  Hanover bars and the V switch were checked on the rotted copy at 13 dB against a good copy
  the same day: below 0.0002 of full scale, no line loses its switch.  Reusable: the method,
  independent copies of a programme as a dropout truth, works for false alarms; it cannot
  label the defects a disc shares with no other copy, so a hit rate needs a disc known to be
  scratched and a second pressing of it.
- 2026-11-21: the SDTV black level sat 0.08 sigma of the blanking noise too high on every
  disc: `RobustNoise::median` returned the upper of the two middle values on an even count,
  and the porch windows it measures hold 16 samples, so each row's clamp level and the
  frame's blanking estimate were biased by that much (0.4 IRE on a 27 dB capture, 1.7 IRE on
  a rotted PAL disc at 13 dB), and the white-flag gain on NTSC the other way.  Found while
  comparing the decoder's rescaled frame against its own raw composite dump (a comparison
  between copies of a disc is no use: three masterings of one PAL programme differ by 4 % of
  range between themselves).  The median is the mean of the two middle values now; all 53
  regression goldens re-recorded (NTSC 59-65 dB against the old ones, the WOW text clip 43.8
  as its gain moved, PAL 50-56, MUSE 65-71).
- 2026-11-20: the MUSE motion detection.  The detector was the two-frame difference alone
  (frames N and N-2, the same sub-sampling phase), cored by a threshold that grew with the edge
  signal and the brightness.  Judged offline against the decoder's own intra-field decode (the
  difference between consecutive intra-field decodes of the same parity is alias-free, so it is
  a true change between the two frames the still path weaves; a candidate map's picture is the
  linear mix of the intra and inter-frame decodes, which is what the combine shader does), it
  missed most slowly panning outlines on Ursa Minor Blue -- beaded, dashed lines where the still
  path was woven over motion -- and on that 2:3 pulldown film called two frames in five moving
  where frame N-2 is another film frame but the weave partner N-1 is the same one.  The
  detector is now the NHK book's one-frame low-band detection (section 4.14): a 3 MHz
  low-pass of frame N minus that of frame N-1, the vertical [1 2 1] over the field's lines,
  rectified and cored from 2 to 5 grey levels, with the two-frame difference kept as the
  book's supplement for small high-frequency objects at a plain ramp from 8 to 16.  The two
  frames' samples are offset by half a sample whose side alternates line by line (the
  frame-offset pattern is a line-offset pattern within a field) and flips with the frame phase,
  so frame N-1 is read through the other polyphase of the same kernel from the side the row's
  phase says; without that the vertical [1 2 1] alone cancels the offset to first order (a
  still edge's residual 1.4 instead of 1.2 grey levels at 3 MHz; without the vertical filter
  5), which is why the book insists on it.  Measured on Ursa Minor Blue per field, pixels of
  the field grid where the reference sees a change of 3 or more grey levels: missed 23800 to
  14100, falsely moving 3400 to 3800; the normal decode's distance from the intra decode in
  moving regions 118k to 80k grey levels summed, from the inter-frame decode in still regions
  13.5k to 16.7k.  On the Sony test disc's still pattern the new detector gives a partial mix
  on 0.5 % of the pixels, along the colour bars' hard vertical edges, invisible.  Cost on the
  M2: the four passes 560 to about 310 us per field (the 9x4 difference window of the old
  pass 2 became a horizontal pass); on the Apple TV's A10X about 0.4 ms more per field of the
  decoder's 10 and 7.7 (fields 0 and 1, Sony test disc) at an unchanged 29.96 frames/s, and
  an interior path without the column clamps changed nothing measurable, so it was not kept.
- 2026-11-20: the MUSE chroma reconstruction filters.  The still path gathers four fields into
  row pairs, two adjacent frame lines from the two fields, whose rows hold complementary
  columns; `filter_c` filled each row's missing columns from the row's own neighbours, which
  halved the horizontal chroma resolution and quantised every vertical chroma edge a column
  apart in the two rows, a line-alternating sawtooth on every such edge (the V plane of Ursa
  Minor Blue shows it on every outline).  Now the missing column is the other row's sample, the
  book's inter-field interpolation, which the line-sequential chroma's vertical band (an eighth
  of a cycle per line) makes as good as the row's own.  The still path's 1:3 chroma upsampler
  is a Catmull-Rom cubic (a triangle before) and the moving path's chroma upsampler a separable
  cubic over 4x4 samples (bilinear).  Measured on the written file's U/V planes (point-sampled
  from the decoder's chroma, so a chroma-plane pixel is two luma pixels): the still path's
  horizontal edges 3.1-4.1 to 2.0-2.7 chroma pixels, which is 1.3-1.9 of the raw chroma
  samples, the signal's own limit; the moving path's 2.7-5.1 to 2.7-4.8 horizontally and
  2.6-3.1 to 2.1-2.65 vertically; a thin orange pole in Ursa Minor Blue keeps 29 units of
  chroma amplitude instead of 22.5.  Cost: the partner-row fill and the cubic 1:3 nothing
  measurable, the moving path's cubic about 15% of the combine shader (835 to 976 us per field
  on the M2).  Left as it was: the vertical fill between the two colours' row pairs, a linear
  interpolation the chroma's vertical band justifies.
- 2026-11-20: the MUSE luma reconstruction filters, measured on the Sony test disc's wedges and
  sweep (the stripes' contrast at a known frequency, fitted as a sine, against the flat areas'
  black-to-white swing): the quincunx diamond interpolator is the windowed ideal interpolator,
  7 lines by 13 pixels, in place of the 7x7 kernel from Vetterli's paper, which passed 0.60 at
  0.3 cycles per line and turned the rest into the stripes' alias -- a half-band interpolator
  wastes nothing, what it does not pass comes back as a checkerboard on the detail, the beaded
  texture on fine stripes and diagonals, 2.2 grey levels on the sweep's fine end and 0.06 after.
  The resolution limit itself did not move (the encoder's), the contrast at the limit rose by
  a tenth on the still path and a quarter on the moving one; the moving path's vertical
  interpolation is a cubic in place of the two-line average.  Costs: 301 against 161 us per
  still-path field on the M2, 1.0 ms per frame of the decoder's 16 on the Apple TV's A10X at an
  unchanged 30 frames/s (measured there from the decoder's per-field log; MoltenVK's
  per-dispatch timestamps read 40-60 us for every dispatch on that GPU and are no use).  Tried
  and rejected: a sharper 2:3 filter for the moving path (cutoff 0.16 instead of 0.13), which
  tripled the flat areas' texture.  Not touched: the chroma filters and the motion detection.
- 2026-11-19: a player's baseband output carries the MUSE signal as transmitted, both emphases
  intact, so museld's de-emphasis stages apply to it as to RF: the HIL-C1's 16.2 MHz output and
  the X9's RF of the same disc (A River Runs Through It, the Pioneer title card, a static
  white-on-black frame, 16 same-phase frames averaged on each side) agree sample for sample
  with the RF's emphasised samples (residual 4.8 codes after a gain and offset fit, below the
  single-frame noise), not with the RF after the linear de-emphasis (10.3; the edge profiles
  show it: both have the pre-edge dip and the overshoot of the emphasis, the de-emphasised
  RF neither), and where the non-linear stage acts (2500 of 35000 samples, by 11 codes rms)
  the RF before it matches better than after it (9.2 against 12.9).  The reader's crude
  level fit on such input (the 500th sample from each end of two frames to 16 and 239) does not
  matter: the decoder rescales every frame from the signal's own reference lines, and the two
  decoded pictures agree within 8 %.  The L key and the menu's non-linear de-emphasis toggle,
  which existed for this question, are gone.
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
- 2026-11-12: `--write-audio analog,efm,ac3` writes the listed tracks as separate audio streams
  (2026-11-13: `muse,efm` on MUSE, checked on a MUSE capture with an EFM track: the EFM stream
  of the two-track file is bit-identical to the `--efm` render).
- 2026-11-13: the MUSE audio sync search leaves the pattern for the frame decode instead of
  consuming it, so a sync found at the tail of a pass no longer costs two misdecoded frames and
  three warnings on the next.
- 2026-11-14: going to a chapter, a CLV time or a CAV frame by number: select on the menu's
  Time/Frame and Chapter lines, which show the position (a digit cursor driven by the remote's five keys, or typed), the G key, `--time`
  and `--frame`, on the chapter search generalised to a position search.
