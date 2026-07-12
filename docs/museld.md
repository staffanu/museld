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
| `--no-dropout` / `--highlight-dropout` | Disable dropout concealment, or highlight dropouts instead of concealing them (see below) |
| `--write-muse16 <file>` | Re-encode input to the 16.2 MHz format (see below) |
| `--write <file>` | Write decoded video and audio to a media file (requires FFmpeg ≥ 7.1) |
| `--write-preset <preset>` | Media file preset: `standard` (H.264 + AAC in MP4, default) or `archival` (lossless FFV1 16-bit + PCM in Matroska). Container and codecs come from the preset, not the filename extension. |
| `--log <spec>` | Log level per category, e.g. `A3V4` (Audio=Info, Video=Debug). Categories: M P A V D I O; levels 0–4 = Off Error Warn Info Debug |
| `--benchmark-shaders` | Print GPU shader timing statistics |
| `--eq <mode>` | MUSE adaptive equaliser mode: `on` (default, taps adapt continuously via LMS), `frozen` (use current taps without further adaptation), `off` (bypass the equaliser) |
| `--subtitles <file.srt>` | Display SRT subtitles synced to the disc's own time code |
| `--subtitle-font <file.ttf>` | Override the default subtitle font (bundled Noto Sans JP) |

**Dropouts** (RF input): the default is to conceal detected dropouts by averaging the surrounding
picture lines. `--no-dropout` leaves them untouched; `--highlight-dropout` paints them red
(dropout in the luminance area) or green (dropout in the color area). The D key cycles between
the three modes during playback.

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
| D | Cycle dropout handling: conceal → ignore → highlight (red = luminance dropout, green = color dropout) |
| A | Toggle audio between MUSE and EFM (RF input only) |
| V | Toggle disc code / chapter / frame display (TOC reading is not implemented) |
| C | Show cursor coordinates and input-file offsets (see below) |
| L | Toggle non-linear de-emphasis processing |
| Z | Cycle zoom: 1× → 2× → 4× (arrow keys pan when zoomed) |
| Print Screen | Copy the displayed cursor/offset text to the clipboard |

The C key overlay shows the cursor position both in single-field coordinates (374×516) and in the
decoded picture (1122×1032). When playback is paused and intra-field interpolation is forced (key
2), it also shows calculated input-file offsets for the pixel under the cursor: the frame start,
the field start (the start of the sound data, i.e. the third line of the field), and the Y, Cr, and
Cb sample offsets. This is useful for locating a dropout in the input RF file when working on
dropout detection — the offsets are off by something like a thousand samples for unknown reasons,
but usually close enough to find the dropout.

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
- **RF sample rate**: EFM input filters are hard-coded for 62.5 MHz; other sample rates require filter recomputation.
- **Non-linear de-emphasis**: Unclear whether it has already been applied on baseband (player output) input; use the L key to toggle.
- **EFM de-emphasis**: Not yet implemented; needed for discs with emphasis flag set, if any exist.
- **Audio channel mapping**: Four-channel MUSE audio channel assignment may vary across audio backends and device configurations.
- **Direct USB input**: Currently requires an external capture program writing to a FIFO (e.g., picostream).
- **Picture filters**: Most filters were computed early in the project; picture quality could probably be improved by spending more time on them.
- **Motion detection**: The algorithm is quite simplistic and could probably be improved.
- **LD MUSE vs BS MUSE**: Available documentation describes the satellite broadcasting standard. MUSE decoders have separate inputs for the two signals, so presumably there is some difference, but it is unknown what it is.

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

Licensed under the [GNU General Public License v3](../gpl-3.0.txt).
