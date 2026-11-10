# ldstack

Offline companion to museld for experiments on several RF captures of the
same laserdisc pressing: it finds every video line in a capture, identifies
the disc pictures, and resamples the RF of each line onto a fixed grid so
the captures can be aligned, compared and stacked sample by sample.  The
first use is a disc-rot study: two or more good copies stacked into a
reference, and a rotted copy of the same pressing measured against it.

The second use is an EFM test bench: a CIRC encoder predicts a disc's exact
EFM symbols from its known audio, so the decoder's symbol decisions, C1
stage and framing can be scored against the truth (see below).

It is CPU-only and self-contained (no Vulkan, no museld code).

## Build

    cmake -DCMAKE_BUILD_TYPE=Release -S ldstack -B ldstack/build
    cmake --build ldstack/build

## Commands

    ldstack scan <capture.lds> <prefix> [--pal|--ntsc] [--seconds S] [-v]

Demodulates the video FM carrier, slices the sync pulses, chains the
horizontal syncs onto an integer line lattice and smooths their times with a
local quadratic fit over ±12 lines, so that disc wow is followed while pulse
timing noise and dropouts average out.  Vertical sync groups anchor the
frame line numbering (field 1 starts on a line boundary, field 2 half a line
in), and the Philips codes on VBI lines 16-18 of both fields give the picture
number (CAV) or the time code and picture-in-second (CLV).  Runs at about
1.4x the capture's duration.  Writes:

- `<prefix>.frames.tsv` — one row per frame: sequence number, lattice line of
  line 1, start in input samples, mean samples per line, lines that had their
  own sync pulse, the *picture key* and the VBI text and code words.  The
  picture key is comparable between captures of the same disc: the CAV
  picture number, or for CLV `seconds * 100 + picture`.
- `<prefix>.lines.f64` — per frame, `lines_per_frame + 1` doubles: the start
  of every line and the end of the last, in input samples (NaN when unknown).

Note that a PAL disc played on a player with an NTSC-referenced spindle servo
runs 0.7 % fast (the player locks the disc's 15.625 kHz line rate to 15.734
kHz); the scan measures the line period from the capture, so this only shows
in the reported samples per line.

    ldstack extract <capture.lds> <prefix> <out> [--pictures A-B]
                    [--samples-per-line N] [--stats-only]

Resamples the raw RF of every line onto `N` samples per line (default 2560,
Lanczos-4) and writes it as int16 (16 x the 10-bit input) to `<out>.rf.i16`,
frame after frame, line after line, with `<out>.idx.tsv` giving each frame's
picture key and byte offset.  `<out>.stats.f64` holds four doubles per line:
frame sequence, line, line period in input samples, and the RMS of the RF
over the line.  The period is the wow signal: its once-per-revolution
component (from disc eccentricity) gives the rotation rate, hence which
lines lie on neighbouring tracks.  `--pictures` selects a picture key range,
`--stats-only` skips the RF file.

    ldstack stack <out> <rf1> <rf2> [<rf3> ...] [--max-lag N] [--min-cc C] [--window W] [-v]

For captures of the *same pits* (one stamper): the extracted RF files (the
`<out>` prefixes of `extract`) are aligned to the first one per line by a
sub-sample lag from broadband cross-correlation and a gain match, then
averaged.  Dropouts are left out per window (default 64 samples): a capture
whose window RMS is below 0.6 of the best capture's in that window does not
contribute there.  A line whose correlation with the reference is below
`--min-cc` (default 0.5) is left out entirely.  Output has the layout of
`extract` plus `<out>.stack.f64` (per line: seq, line, then per input lag,
correlation, gain, dropout fraction).

    ldstack stackbb <out> <rf1> [<rf2> ...] [--carrier HZ] [--max-lag N] [--min-cc C] [--window W] [-v]

For captures of *different masterings* of one programme (same video, but
independent FM carrier, audio and EFM, so their RF cannot be stacked): every
line is FM demodulated (default carrier 7.1 MHz, output at half the RF rate),
aligned to the reference per line on the frequency deviation, and averaged
with dropout windows from the RF envelope left out.  Output `<out>.bb.f32`
holds the instantaneous frequency in Hz; with one input this is plain
demodulation, e.g. of a rotted copy for comparison with a stacked reference.

## EFM test bench

The second half scores museld's EFM decoder against the truth.  `src/CircEncoder.cpp` is the
exact mirror of museld's CIRC decoder (IEC 60908): given a disc's audio as s16le stereo PCM (a
clean decode, or a verified CD rip of the same disc), it predicts the 32 data and parity symbols
of every EFM frame, so each stage of the decoder can be checked symbol by symbol.  The *phase*
0-5 is which stereo sample starts frame 0; the commands search it unless `--phase` fixes it.
`--range MIN-MAX` limits the scoring to a range of received frames.

    ldstack efmpredict <audio.pcm> <circ-dump.txt> [--range MIN-MAX] [--report FILE]

Compares the predicted symbols with the frames an `ac3rf-efm-decode --circ-debug-filename` dump
received (its `F:` records).  Finds the frame phase and the offset between predicted and
received frame numbers, tracks the alignment block by block (a change of offset is a frame
the decoder lost or invented), and reports the match over the whole capture and the erased
symbol fraction.  `--report` writes every frame with its predicted and received symbols and the
erasure mask.

    ldstack efmc1 <audio.pcm> <circ-dump.txt> [--phase P] [--range MIN-MAX]

Scores the C1 stage: from the dump's `C1:` (input word) and `D1:` (after C1) records and the
predicted words, every C1 output is classified as correct, erased (some symbols left flagged)
or miscorrected (an unflagged symbol that is wrong), with the input errors per word.  This is
how the `--c1-strategy` variants were compared: a miscorrection here is what C2 may let
through to the audio.

    ldstack efmscore <audio.pcm> <symbols.f32> [--phase P] [--range MIN-MAX] [--dump-symbols FILE]

Scores the per-symbol decisions in an `ac3rf-efm-decode --symbol-debug-filename` dump: how
many 14-bit table hits are wrong (a valid code that is not the one sent), how often the ML
estimate is right when the table misses, and how well its score margin separates right from
wrong decisions (percentiles of the margin for each class).  `--dump-symbols` writes the
per-symbol verdicts with their alignment.

    ldstack efmbits <audio.pcm> <tvalues.u8> <phase> <received_frame:predicted_frame>
                    [--window BITS] [--search BITS] [--min-score F] [--trace FILE]

Aligns the predicted channel bits (merging bits and the subcode symbol are unknown and
skipped) with the transition stream of an `ac3rf-efm-decode --t-values-output-filename` dump,
window by window (default 1176 bits, two frames), following bit clock slips, from a hint of
one received frame and the predicted frame it holds (`efmpredict` gives it).  Reports the
windows lost, the predicted transitions found in place, shifted early or late or missing, the
spurious received transitions, the detection rate by adjacent run length, the confusion matrix
of received against predicted run length, and the mean run-length error by polarity class:
the deformation of the eye, measured on the sliced bits.

    ldstack efmexport <audio.pcm> <phase> <g0> <g1> <out>

Writes the predicted channel bits of frames g0..g1 as two byte arrays, `<out>.toggle.u8` and
`<out>.known.u8`, for other tools.

A caution learned here: a 14-bit window cut at the wrong bit alignment is still a valid EFM
code about 95 % of the time, so "valid symbol" never means "aligned"; and the t-values file
normalises run lengths to 3-11, so bit-level work needs the symbol dump, not the t-values.

## What "same pressing" means here

Two discs share their pits only if they come from one master.  The test is
band-wise coherence of the line-locked RF: with the same pits the EFM band
(0.3-1.2 MHz) correlates at 1.0 window by window, with different masterings
it sits at the 0.4-0.5 noise floor while the demodulated video still
correlates at 0.99.  In the first sample set only the two D515 captures
(NYCSTM4FR, NYCSTM5GB) share pits; the three LD-V4400 discs are three
masterings of the same programme.

## Dropped-frame and lattice conventions

The lattice line number `k` counts sync periods from the first accepted pulse
and stays continuous across the vertical interval, where no horizontal sync
pulses exist (the ~15 lines without a pulse of their own are interpolated
from the fit).  A frame is 625 (PAL) or 525 (NTSC) consecutive lattice lines
from a field-1 vertical group; a group that does not fall on the lattice is
skipped.
