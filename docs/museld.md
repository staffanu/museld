# museld — Player Reference

## Quick start

Download a sample RF capture and play it:

```console
wget --directory-prefix ../data/muse https://madeye.org/muse-demo/makeup-muse-rf-62.5MHz-nofilter.raw
./player/build-muse/src/musecpp --demodulate ../data/muse/makeup-muse-rf-62.5MHz-nofilter.raw
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

## Input options

| Option | Description |
|---|---|
| `--input-format <fmt>` | Input sample type: `u8`, `s8`, `u16`, `s16`, `lds`, `flac`, `ldf`. Auto-detected from filename extension when omitted. |
| `--input-type <type>` | Input type: `muse-rf`, `ntsc-rf`, `muse-16`, `muse-os`. RF input MUSE/NTSC, phase correct 16.2 MHz MUSE baseband, oversampled MUSE baseband. Default is `muse-rf`. |
| `--sample-freq <Hz>` | Sets the input sample rate. Default 62.5 MHz. |

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
| `--no-dropout` / `--highlight-dropout` | Suppress or highlight dropout concealment |
| `--write-muse16 <file>` | Re-encode input to 16.2 MHz little-endian format |
| `--write <file>` | Write decoded video to media file (FFmpeg, experimental) |
| `--log <spec>` | Log level per category, e.g. `A3V4` (Audio=Info, Video=Debug). Categories: M P A V D I O; levels 0–4 = Off Error Warn Info Debug |
| `--benchmark-shaders` | Print GPU shader timing statistics |

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
| D | Cycle dropout handling: conceal → ignore → highlight |
| A | Toggle audio between MUSE and EFM (RF input only) |
| V | Toggle disc code / chapter / frame display |
| C | Show cursor coordinates in field and decoded picture space |
| L | Toggle non-linear de-emphasis processing |
| Z | Cycle zoom: 1× → 2× → 4× (arrow keys pan when zoomed) |

## Architecture

### Data flow — MUSE

```
RF capture (62.5 MHz) → MuseRfDemodulator → ResamplingInputReader (DPLL, 16.2 MHz)
  → FrameBuffer → Vulkan GPU shaders (de-emphasis, gamma, color decode, motion detection)
  → GLFW window + PortAudio
```

The resampling DPLL runs in a dedicated CPU thread. Vulkan command recording and GLFW event handling
run on the main thread. The GPU pipeline has two stages separated by a semaphore.

### Data flow — NTSC

```
RF capture (40 MHz) → NtscRfDemodulator → NtscInputReader (DPLL)
  → NtscFrame → Vulkan GPU shaders (sync burst detection, color filtering, field decode)
  → GLFW window
```

### Data flow — EFM (inside musecpp)

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
- **Audio channel mapping**: Four-channel MUSE audio channel assignment may vary across PortAudio configurations.
- **Direct USB input**: Currently requires an external capture program writing to a FIFO (e.g., picostream).

## References

- *MUSE－ハイビジョン伝送方式*, Yuichi Ninomiya, 1990
- *[High Definition Television Hi Vision Technology](https://archive.org/details/high-definition-television-hi-vision-technology)*, NHK STRL, 1993
- *An HDTV Broadcasting System Utilizing a Bandwidth Compression Technique-MUSE*, Seiichi Gohshi, 1988
- ITU-R BO.786, *MUSE system for HDTV broadcasting-satellite services*, 1992
- EP0532277A2, *Method of recording information on video disk* (Disc Code / TOC)

## Author and License

Software written by Staffan Ulfberg 2021–2026.

Licensed under the [GNU General Public License v3](../gpl-3.0.txt).
