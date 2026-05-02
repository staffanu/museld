# museld — Real-time MUSE/NTSC Laserdisc Player

This repository contains a real-time decoder for Hi-Vision MUSE and NTSC laserdiscs, along with a
reusable library and CLI for decoding AC3-RF surround audio and EFM CD audio from laserdisc RF captures.

If you want to decode laserdiscs in general, the [ld-decode project](https://github.com/happycube/ld-decode)
is probably a better place to start, unless you are specifically interested in real-time MUSE decoding.

## Components

### musecpp

A complete real-time [MUSE](https://en.wikipedia.org/wiki/Multiple_sub-Nyquist_sampling_encoding) and NTSC
laserdisc decoder. Video filtering runs on the GPU via Vulkan and GLSL compute shaders. Audio is output via
PortAudio.

Input can be:
- A MUSE player's baseband output digitized at 16.2 MHz (little-endian 16-bit shorts)
- Raw RF captured directly from the optical pickup, digitized at 62.5 MHz (unsigned 16-bit)
- An NTSC RF capture (40 MHz, unsigned 16-bit)

EFM CD audio (from the pits on the disc) is decoded alongside the video when `--efm` is specified.

### ac3rf-efm-decode

A CLI and reusable C++ library for decoding:
- **EFM**: CD audio stored on laserdisc, using CIRC Reed-Solomon error correction
- **AC3-RF**: QPSK-modulated AC3 surround audio at 2.88 MHz carrier

Supports `.lds` (10-bit packed) and `.ldf` (FLAC-in-Ogg) input formats, plus raw uint8/16/sint8/16.

### fl2kmuse

Standalone test signal generator for MUSE signals via an FL2K USB device.

### picostream

Standalone C wrapper for streaming data from a Picoscope 5000-series oscilloscope to a file or FIFO.
Used to capture MUSE RF in real time for piping directly into musecpp.

## Building

All components require CMake 3.22+, a C++23 compiler, and the vendored firpm submodule.

```bash
git submodule update --init --recursive
```

### ac3rf-efm-decode (minimal dependencies)

Dependencies: FLAC++ (`libflac++`), Eigen3, pkg-config.

```bash
cmake -DCMAKE_BUILD_TYPE=Release -B build-release .
cmake --build build-release

# With AddressSanitizer
cmake -DCMAKE_BUILD_TYPE=Debug -B build-debug .
cmake --build build-debug
```

### musecpp (adds Vulkan, GLFW, PortAudio, GNURadio)

Ubuntu packages:
```bash
sudo apt install cmake glslc libglfw3-dev portaudio19-dev libavformat-dev libavcodec-dev \
    libswscale-dev catch2 vulkan-tools vulkan-validationlayers-dev gnuradio-dev
```

macOS (Homebrew): packages have similar names; use Homebrew for dependencies.

```bash
cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_MUSE=ON -B build-muse .
cmake --build build-muse
```

GLSL shaders in `src/shaders/` are compiled to SPIR-V by `glslc` at build time.

### fl2kmuse (standalone, requires libosmo-fl2k)

```bash
cmake -DCMAKE_BUILD_TYPE=Release -S fl2kmuse -B fl2kmuse/build-release
cmake --build fl2kmuse/build-release
```

### picostream (standalone, requires Picoscope SDK at /opt/picoscope)

```bash
cmake -DCMAKE_BUILD_TYPE=Release -S picostream -B picostream/build-release
cmake --build picostream/build-release
```

## Running tests

```bash
cmake -DCMAKE_BUILD_TYPE=Debug -B build-debug .
cmake --build build-debug
cd build-debug && ctest -V
```

## Running musecpp

Quick start — download a sample RF capture and play it:
```console
wget --directory-prefix ../data/muse https://madeye.org/muse-demo/makeup-muse-rf-62.5MHz-nofilter.raw
./build-muse/src/musecpp --demodulate ../data/muse/makeup-muse-rf-62.5MHz-nofilter.raw
```

![MUSE test picture](src/test-picture.png)

### Hardware compatibility

Performance is measured as MUSE frames per second; 30 fps is required for real-time playback. The picture
is updated 60 times per second (once per field) unless `--full-frames-only` is used.

| Machine | Performance |
|---|---|
| Apple MacBook Pro M2 (8/10 core) | 40+ fps, works fine |
| Lenovo ThinkPad X1 Carbon Gen 9 (i7-1185G7) | 70+ fps cold, drops near 30 fps when throttling |
| Asus PN51 Mini PC (Ryzen 3 5300U) | works fine (main playback machine) |
| Apple iMac 27" 2017 (Radeon Pro 580) | 80+ fps |

Machines tested and found too slow: NVIDIA Jetson Nano, Raspberry Pi 5 (1–5 fps).

### Input format options

| Option | Description |
|---|---|
| `--resample-shorts` | Oversampled input, 16-bit signed little-endian (e.g., 62.5 MHz RF, use with `--sample-freq`) |
| `--resample-bytes` | Oversampled input, unsigned bytes |
| `--big-endian` | 16.2 MHz baseband, 10-bit in 16-bit big-endian shorts |
| `--little-endian` | 16.2 MHz baseband, 10-bit in 16-bit little-endian shorts (preferred) |
| `--demodulate` | RF input at 62.5 MHz (optical pickup) — enables RF demodulation |
| `--sample-freq <Hz>` | Sets the input sample rate for RF/oversampled formats |

### Playback options

| Option | Description |
|---|---|
| `--efm` | Use EFM data for audio instead of MUSE audio (RF input only) |
| `--fifo` | Input is a FIFO; synchronize playback speed to incoming data rate |
| `--no-video` / `--no-audio` | Disable video or audio output |
| `--no-sync` | Display frames as fast as possible (benchmark mode) |
| `--full-frames-only` | Skip every other field update (reduces CPU/GPU load) |
| `--all-fields` | Update display at 60 Hz (default) |
| `--full-screen` | Start full screen |
| `--seek <seconds>` | Seek to position before starting playback |
| `--pause` | Start paused |
| `--no-dropout` / `--highlight-dropout` | Suppress or highlight dropout concealment |
| `--write-muse <file>` | Re-encode input to 16.2 MHz little-endian format |
| `--write <file>` | Write decoded video to media file (FFmpeg, experimental) |
| `--log <spec>` | Log level per category, e.g. `A3V4` (Audio=Info, Video=Debug). Categories: M P A V D I O; levels 0–4 = Off Error Warn Info Debug |
| `--benchmark-shaders` | Print GPU shader timing statistics |

### Keyboard shortcuts during playback

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

## Architecture overview

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

### Data flow — EFM (inside musecpp and ac3rf-efm-decode)

```
RF → TimingRecovery (Mueller-Müller PLL) → EfmDecoder → CIRC C1/C2 Reed-Solomon
  → erasure concealment → pop detection → stereo PCM
```

### Data flow — AC3-RF

```
RF → bandpass FIR → decimation → IQ mixing (DPLL) → QPSK demodulation
  → frame sync → de-interleave → Reed-Solomon C1/C2 → AC3 bitstream
```

AC3-RF uses two RS codes over GF(2^8) with irreducible polynomial 0x187 and primitive element 2:
C1 is a (37,33) code and C2 is a (36,32) code with first consecutive root 2^120.

## Known limitations and future work

- **Motion vectors**: MUSE specifies motion compensation in control data (3+4 bits), but these bits are
  always zero on every disc tested. It is unclear whether this feature was ever used on MUSE laserdiscs.
- **RF sample rate**: EFM input filters are hard-coded for 62.5 MHz; other sample rates require filter recomputation.
- **Non-linear de-emphasis**: Unclear whether it has already been applied on baseband (player output) input; use the L key to toggle.
- **EFM de-emphasis**: Not yet implemented; needed for discs with emphasis flag set.
- **Audio channel mapping**: Four-channel MUSE audio channel assignment may vary across PortAudio configurations.
- **Direct USB input**: Currently requires an external capture program writing to a FIFO (e.g., picostream).

## Author and License

Software written by Staffan Ulfberg 2021–2026.

Licensed under the [GNU General Public License v3](gpl-3.0.txt).

## References (MUSE)

- *MUSE－ハイビジョン伝送方式*, Yuichi Ninomiya, 1990
- *[High Definition Television Hi Vision Technology](https://archive.org/details/high-definition-television-hi-vision-technology)*, NHK STRL, 1993
- *An HDTV Broadcasting System Utilizing a Bandwidth Compression Technique-MUSE*, Seiichi Gohshi, 1988
- ITU-R BO.786, *MUSE system for HDTV broadcasting-satellite services*, 1992
- EP0532277A2, *Method of recording information on video disk* (Disc Code / TOC)
