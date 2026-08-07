# museld — Player Reference

## Quick start

Download a sample RF capture and play it:

```console
wget --directory-prefix ../data/muse https://madeye.org/muse-demo/makeup-muse-rf-62.5MHz-nofilter.raw
./player/build-release/src/museld ../data/muse/makeup-muse-rf-62.5MHz-nofilter.raw
```

![MUSE test picture](../test-picture.png)

## Hardware compatibility

Performance is measured as MUSE frames per second; 30 fps is required for real-time playback. The picture
is updated 60 times per second (once per field) unless `--full-frames-only` is used.

| Machine | Performance |
|---|---|
| Apple MacBook Pro M2 (8/10 core) | 40+ fps, works fine |
| Lenovo ThinkPad X1 Carbon Gen 9 (i7-1185G7) | 70+ fps cold, drops near 30 fps when throttling |
| Asus PN51 Mini PC (Ryzen 3 5300U) | works fine (main playback machine) |
| Apple iMac 27" 2017 (Radeon Pro 580) | 80+ fps |

Machines tested and found too slow: NVIDIA Jetson Nano, Raspberry Pi 5 (1–5 fps).

The numbers were obtained by playing an RF capture with `--no-sync --log P4`. There are two
bottlenecks: GPU performance, and the resampling to 16.2 MHz (a software DPLL) that runs in a
single CPU thread.

## Input options

| Option | Description |
|---|---|
| `--input-format <fmt>` | Input sample type: `u8`, `s8`, `u16`, `s16`, `u16be`, `s16be`, `lds`, `flac`, `ldf`. Auto-detected from filename extension when omitted. |
| `--input-type <type>` | Input type: `muse-rf`, `ntsc-rf`, `muse-16`, `muse-os`. RF input MUSE/NTSC, phase correct 16.2 MHz MUSE baseband, oversampled MUSE baseband. Default is `muse-rf`. |
| `--sample-freq <Hz>` | Sets the input sample rate. Default 62.5 MHz. |

Any argument not starting with `-` is an input filename. Several files can be given, with other
options in between; each file is played with the most recent options in effect, so files with
different formats can be played back to back. An argument starting with `!` is ignored, which is
practical for temporarily disabling options when reusing long command lines.

Because the options in effect are the ones seen so far, **every option must come before the file
it applies to**. An option placed after a filename applies only to later files, and options after
the last filename have no effect at all. That cannot be an error, since it is the same syntax that
gives a second file different settings, so museld plays the files and warns about the leftovers
afterwards:

```console
# both files play with --no-dropout; the second one is also NTSC at 40 MHz
museld --no-dropout muse.ldf --input-type ntsc-rf --sample-freq 40e6 ntsc.ldf

# WRONG: --no-dropout comes too late, and museld says so
museld muse.ldf --no-dropout
#   Warning: this option came after the last input file and had no effect: --no-dropout
```

An argument disabled with a leading `!` is not reported, since ignoring it is the point.

If an input file is a FIFO (named pipe), museld assumes the data is produced in real time, e.g. by
a capture program such as picostream. Playback speed then follows the incoming data rate: an empty
pipe means waiting rather than end of file, a field is dropped if the input buffers fill up, and
the OS pipe buffer size is increased (Linux). Seeking is not possible with FIFO input.

## Playback options

| Option | Description |
|---|---|
| `--efm` | Use EFM data for audio instead of MUSE audio (RF input only) |
| `--no-video` / `--no-audio` | Disable video or audio output |
| `--no-sync` | Display frames as fast as possible (benchmark mode) |
| `--full-frames-only` | Skip every other field update (reduces CPU/GPU load) |
| `--all-fields` | Update display at 60 Hz (default) |
| `--full-screen` | Start full screen |
| `--seek <seconds>` | Seek to position before starting playback |
| `--pause` | Start paused |
| `--export-frame <file>` | Save one decoded frame as PNG to `<file>` and quit. Combine with `--export-frame-at` to give the decoder (DPLL, adaptive equaliser, motion detection) a warm-up run from the `--seek` position. |
| `--export-frame-at <seconds>` | Stream position of the frame saved by `--export-frame` (absolute, same units as `--seek` — not a delay after it). Default: the first decoded frame. |
| `--no-dropout` / `--highlight-dropout` | Disable dropout concealment, or highlight dropouts instead of concealing them (see below) |
| `--write-muse16 <file>` | Re-encode input to the 16.2 MHz format (see below) |
| `--write <file>` | Write decoded video and audio to a media file (requires FFmpeg ≥ 7.1) |
| `--write-preset <preset>` | Media file preset: `standard` (H.264 + AAC in MP4, default) or `archival` (lossless FFV1 16-bit + PCM in Matroska). Container and codecs come from the preset, not the filename extension. |
| `--write-duration <seconds>` | Stop after writing this much video and finalize the file (default: run to the end of the input) |
| `--log <spec>` | Log level per category, e.g. `A3V4` (Audio=Info, Video=Debug). Categories: M P A V D I O; levels 0–4 = Off Error Warn Info Debug |
| `--benchmark-shaders` | Print GPU shader timing statistics |
| `--eq <mode>` | MUSE adaptive equaliser mode: `on` (default, taps adapt continuously via LMS), `frozen` (use current taps without further adaptation), `off` (bypass the equaliser) |
| `--field-interpolation <mode>` | Initial de-interlacing mode: `normal` (motion-adaptive), `intra-field`, or `inter-frame` — same as keys 1/2/3 |
| `--no-3d-comb` | NTSC: start with the temporal Y/C separation off (spatial 3-line comb everywhere) — same as key 4 |
| `--no-film-mode` | NTSC: start with the film mode off instead of auto — same as key 5 |
| `--tint <degrees>` | NTSC: rotate the chroma hue. Added to the decoder's calibrated angle; compensates source-dependent differential phase (player and disc), like a TV's tint control |
| `--saturation <factor>` | NTSC: scale the chroma gain (default 1.0, applied on top of the burst-referenced AGC) |
| `--subtitles` | Display SRT subtitles synced to the disc's own time code — or to the playback position when the capture carries no disc code (baseband captures usually do not). The available tracks are the `.srt` files next to the input file whose names start with the input's name minus its extension (`capture.srt`, `capture.ja.srt`, … for `capture.raw`). The first one alphabetically starts as the primary (bottom) track; the `[` and `]` keys cycle the primary and secondary (top) track through all of them |
| `--subtitles-file <file.srt>` | Start with this file as the primary subtitle track (implies `--subtitles`; the file need not match the naming pattern) |
| `--subtitle-offset <seconds>` | Delay the subtitles by this much; negative shows them earlier. Useful for tracks timed against a `--write` render rather than the disc's own time code |
| `--subtitle-font <file.ttf>` | Override the default subtitle font (bundled Noto Sans JP) |

### Live subtitle OCR and translation (experimental)

Discs with burned-in subtitles can be read and translated during playback. This
needs a build with `-DUSE_OCR=ON` (requires ONNX Runtime) and the PP-OCR text
detection and recognition models as `.onnx` files (with "det" and "rec" in
their names) in a directory of your choice.

| Option | Description |
|---|---|
| `--ocr <dir>` | OCR the burned-in subtitles into a live "OCR" subtitle track, selectable like any file track with the `[` and `]` keys. `<dir>` holds the detection and recognition models; the recognition model decides the alphabet (PaddleOCR publishes per-script models: the default Chinese/Japanese one, `latin`, `korean`, `cyrillic`, …) |
| `--ocr-script <script>` | Script a text row must contain to count as a subtitle, dropping film credits and stray detections: `cjk` (default; Japanese and Chinese), `latin`, or `any` (keep everything) |
| `--ocr-translate <url>` | Translate the OCR track into a live translated track via the OpenAI-compatible server at `<url>` (llama.cpp, Ollama, vLLM, …; e.g. `http://localhost:11434`). `$OPENAI_API_KEY` is sent as a bearer token if set. Each cue is translated with a rolling window of the recent dialogue as context |
| `--ocr-translate-model <name>` | Model to use (default: the first one the server reports) |
| `--ocr-language <name>` | Language of the burned-in subtitles, for the translation prompt (default Japanese) |
| `--ocr-translate-to <name>` | Language to translate into (default English). The live track and the saved file are named after its first two letters ("OCR-EN", "OCR-SV", …) |
| `--ocr-write` | Save the collected cues at exit as `<input>.OCR.srt` (and `<input>.OCR-XX.srt` with `--ocr-translate`) next to the input, with the cues' original on-screen timing — `--subtitles` finds them on the next playback, so one watched pass extracts a finished subtitle pair |

The OCR watches the bottom of the frame and runs only when the subtitle area
changes, so the model cost is per cue, not per frame. Text appears on the live
tracks with a small lag (OCR plus translation, typically around a second); a
cue that appeared late is kept up correspondingly longer. Translating into a
language whose script the bundled font does not cover may need
`--subtitle-font`.

**Dropouts** (RF input): the default is to conceal detected dropouts by averaging the surrounding
picture lines. `--no-dropout` leaves them untouched; `--highlight-dropout` paints them red
(dropout in the luminance area) or green (dropout in the color area). The D key cycles between
the three modes during playback.

Both MUSE and NTSC detect dropouts from the RF envelope (`detect_dropouts_envelope.comp`), the way
hardware players do: a dropout is a collapse of the carrier amplitude, whatever the demodulated
value happens to be. The local envelope is compared against the same position on the neighbouring
lines and against a long strided average, so no absolute threshold or per-capture gain calibration
is needed; the more sensitive line-referenced condition additionally requires the demodulated
signal to have gone noisy.

The two paths use quite different thresholds, because the legal envelope variation is
format-specific. MUSE deviates far enough that picture content alone roughly halves the envelope
power between black and peak white, so its ratios are set well below NTSC's — see the comment in
`MuseRfDemodulator.cpp` for the measurements behind them. MUSE previously used a plain value
threshold on the demodulated signal, which could not tell bright detail from a dropout and
concealed white-on-black titles away as if they were dropouts.

There is a lot of room between the two cases, so the thresholds are not a compromise: content
takes the envelope down to about 0.16 of the local median at worst, while the real dropouts in
`makeup-muse-rf-62.5MHz-nofilter.raw` (around 55.5–56.1 s) reach 0.0036. That capture is the one
to reach for when working on dropout handling — it has a disc blemish that is struck once per
revolution for about 18 frames.

**NTSC Y/C separation and de-interlacing**: the decoder banks four composite frames and displays
one frame behind the input (audio is delayed to match). Per-pixel directional motion masks select,
for each side, between temporal Y/C separation against the still neighbouring frames (a 3D comb:
full vertical resolution, no dot crawl) and the 3-line spatial comb where the picture moves. The
same masks drive motion-adaptive de-interlacing: still parts weave the previous field, moving
parts are interpolated from the current field (keys 1/2/3 select adaptive/bob/weave).

**NTSC reverse telecine (film mode)**: film-sourced discs carry 24 fps film as a 3:2 field
cadence, where one field in five repeats the same-parity field of the previous frame. The decoder
detects that cadence from the picture itself (the VBI white flag is written on every frame on real
discs and carries no cadence information) and, while locked, pairs each field with the field it
shares a film frame with: full vertical resolution with none of the motion-adaptive compromises.
Fields that start a new film frame re-show the previous, completed film frame — the presentation
3:2 timing asks for anyway — at the cost of one field of latency. The lock engages after about
half a second of unambiguous cadence, coasts through cuts and static scenes, drops within two
missed repeats when the cadence breaks (film/video splices), and any frame whose expected repeat
is missing falls back to the motion-adaptive path on its own. Lock transitions are logged at
`--log D3`. Key 5 switches between auto and off; it is only active in the `normal` de-interlacing
mode with all fields decoded. The disc code overlay (V key) shows the state: `Film: off`,
`Film: auto` (watching, nothing detected), or `Film: 3:2` while locked. When paused, the locked
line also shows what the displayed field is, e.g. `Film: 3:2 A weave` — the letter is the film
frame within the (A1A2)(A3B1)(B2C1)(C2C3)(D1D2) pulldown cycle the output currently shows (step
with N to watch its 3, 2, 3, 2 rhythm), `hold` marks the fields that re-show the previous film
frame, and `adapt` a locked frame whose missing repeat made it fall back to the motion-adaptive
path.

**NTSC level calibration**: video levels are calibrated automatically against references in the
signal. Black (0 IRE) tracks the measured back porch blanking level; the gain (100 IRE) is taken
from the white flag — a flat 100 IRE line in the vertical interval that most discs carry — when
one is found, and stays at the nominal FM deviation mapping otherwise. The measured levels are
logged with the noise figures (`--log D3`).

**Display rendering**: for display and PNG export, both the MUSE and NTSC paths convert the
decoded signal to linear light (CRT law, gamma 2.2) with SMPTE C primaries mapped to sRGB;
the sRGB swapchain then encodes it for the monitor. The `--write` output is unaffected — it
stays in the signal domain as Y'CbCr, tagged with the source's color metadata.

**`--write-muse16`** writes the input stream in the 16.2 MHz format (each sample stored ×4 as a
little-endian 16-bit value), which is what `--input-type muse-16` reads. This is useful for cutting
small video segments out of larger files, and for re-coding oversampled or RF captures to the much
smaller 16.2 MHz format. Note that when the source is RF, the EFM data is not part of the written
stream and is lost. Combining with `--no-video --no-audio` makes re-coding faster since nothing is
decoded.

**`--write`** renders to a media file; playback is capped at 1.0× realtime unless `--no-sync` is
also given, which is the mode to use for batch rendering.

## Keyboard shortcuts during playback

| Key | Action |
|---|---|
| Q / Escape | Quit |
| Tab | Toggle full screen |
| Space | Pause / resume |
| N | Step one field forward (while paused) |
| Left / Right | Seek ±10 seconds |
| 1 | Normal field interpolation (motion detection) |
| 2 | Force intra-field interpolation (treat everything as motion) |
| 3 | Force inter-frame interpolation (treat everything as still) |
| 4 | NTSC: toggle the 3D comb (temporal Y/C separation on still parts) |
| 5 | NTSC: film mode auto / off (reverse-telecine weave on a 3:2 cadence lock) |
| D | Cycle dropout handling: conceal → ignore → highlight (red = luminance dropout, green = color dropout) |
| A | Toggle audio between MUSE and EFM (RF input only) |
| V | Toggle disc code / chapter / frame display, plus the NTSC film mode status (TOC reading is not implemented) |
| C | Show cursor coordinates and input-file offsets (see below) |
| L | Toggle non-linear de-emphasis processing |
| Z | Cycle zoom: 1× → 2× → 4× (arrow keys pan when zoomed) |
| S | Export the displayed frame to a timestamped PNG in the current directory (without OSD text and subtitles) |
| [ | Cycle the primary subtitle track (bottom of the frame) through the available tracks and off |
| ] | Cycle the secondary subtitle track (top of the frame) through the available tracks and off |
| Print Screen | Copy the displayed cursor/offset text to the clipboard |

The C key overlay shows the cursor position both in single-field coordinates (374×516) and in the
decoded picture (1122×1032). When playback is paused and intra-field interpolation is forced (key
2), it also shows calculated input-file offsets for the pixel under the cursor: the frame start,
the field start (the start of the sound data, i.e. the third line of the field), and the Y, Cr, and
Cb sample offsets. This is useful for locating a dropout in the input RF file when working on
dropout detection — the offsets are off by something like a thousand samples for unknown reasons,
but usually close enough to find the dropout.  For NTSC the offsets are line-accurate (the frame
start is tracked to the input sample), and Y, Cr, and Cb are the same offset since the composite
carries no separate chroma samples.

## Architecture

### Data flow — MUSE

```
RF capture (62.5 MHz) → MuseRfDemodulator → ResamplingInputReader (DPLL, 16.2 MHz)
  → FrameBuffer → Vulkan GPU shaders (de-emphasis, gamma, color decode, motion detection)
  → GLFW window + miniaudio
```

The resampling DPLL runs in a dedicated CPU thread. Vulkan command recording and GLFW event handling
run on the main thread. The GPU pipeline has two stages separated by a semaphore.

### Data flow — NTSC

```
RF capture (40 MHz) → NtscRfDemodulator → NtscInputReader (DPLL)
  → NtscFrame → Vulkan GPU shaders (sync burst detection, color filtering, field decode)
  → GLFW window
```

### Data flow — EFM (inside museld)

```
RF → TimingRecovery (Mueller-Müller PLL) → EfmDecoder → CIRC C1/C2 Reed-Solomon
  → erasure concealment → pop detection → stereo PCM
```

## Known limitations and future work

- **Motion vectors**: MUSE specifies motion compensation in control data (3+4 bits), but these bits are
  always zero on every disc tested. It is unclear whether this feature was ever used on MUSE laserdiscs.
- **Non-linear de-emphasis**: Unclear whether it has already been applied on baseband (player output) input; use the L key to toggle.
- **Direct USB input**: Currently requires an external capture program writing to a FIFO (e.g., picostream).
- **Picture filters**: Most filters were computed early in the project; picture quality could probably be improved by spending more time on them.
- **Motion detection**: The algorithm is quite simplistic and could probably be improved.
- **LD MUSE vs BS MUSE**: Available documentation describes the satellite broadcasting standard. MUSE decoders have separate inputs for the two signals, so presumably there is some difference, but it is unknown what it is.
- **Subtitle OCR** (experimental; see the option section above):
  - Not in the release packages yet: `USE_OCR` is off by default, and how to distribute
    ONNX Runtime and the PP-OCR models with the downloads is undecided (see docs/packaging.md
    before changing this).
  - `--ocr-translate` speaks plain HTTP only — hosted TLS endpoints would need an
    OpenSSL-enabled cpp-httplib build.
  - The live tracks lag the burned-in text by OCR + translation latency (about a second).
    A deliberate presentation-delay buffer (delaying video/audio output a second or two
    behind decode) would make the translated subtitles appear exactly in sync, and would
    also enable temporal inpainting to hide the burned-in text itself.
  - A translation still in flight when playback ends is dropped instead of being flushed
    into the `--ocr-write` file.
  - On Latin-script discs the `--ocr-script latin` filter cannot distinguish subtitles
    from film credits (same script); only the recognition-confidence threshold filters
    noise there.

## References

- *MUSE－ハイビジョン伝送方式*, Yuichi Ninomiya, 1990 (in Japanese)
- *[High Definition Television Hi Vision Technology](https://archive.org/details/high-definition-television-hi-vision-technology)*, NHK STRL, 1993
- *An HDTV Broadcasting System Utilizing a Bandwidth Compression Technique-MUSE*, Seiichi Gohshi, 1988.
  Note: the mapping table from audio ternary symbols to bits in this article is incorrect —
  presumably it was changed after publication.
- *A method of moving area-detection technique in a muse decoder*, Yoshinori Izumi, Seiichi Gohshi, Yuichi Ninomiya, 1993
- ITU-R BO.786, *MUSE system for HDTV broadcasting-satellite services*, 1992
- EP0532277A2, *Method of recording information on video disk* (Disc Code / TOC)

## Author and License

Software written by Staffan Ulfberg 2021–2026.

Licensed under the [GNU General Public License](../gpl-3.0.txt), version 3 or (at your option) any later version.
