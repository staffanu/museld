# museld — Real-time MUSE Laserdisc Player

This repository contains a real-time decoder for Hi-Vision MUSE and NTSC (experimental support)
laserdiscs, along with a reusable library and CLI for decoding AC3-RF surround audio and EFM CD
audio from laserdisc RF captures.

If you want to decode laserdiscs in general,
the [ld-decode project](https://github.com/happycube/ld-decode)
is probably a better place to start, but it doesn't decode MUSE, or
currently, AC3-RF.  For bad captures or damaged discs this EFM decoder
might have better results, but since it's not integrated in the ld-decode
toolchain, try that first.

## Components

### museld

A real-time [MUSE](https://en.wikipedia.org/wiki/Multiple_sub-Nyquist_sampling_encoding)
and NTSC (experimental support) laserdisc decoder. Video filtering runs on the GPU via Vulkan
and GLSL compute shaders. Audio is output via PortAudio.

See **[docs/museld.md](docs/museld.md)** for the full player reference.

### ac3rf-efm-decode

A CLI and reusable C++ library for decoding:
- **EFM**: digital CD audio, using CIRC Reed-Solomon error correction. Works for RF signals from
  Laserdiscs or CD players. DTS on Laserdiscs is also EFM-encoded.
- **AC3-RF**: QPSK-modulated AC3 surround audio at 2.88 MHz carrier, as stored on Laserdiscs.

See **[docs/ac3rf-efm-decode.md](docs/ac3rf-efm-decode.md)** for the CLI reference, and
**[docs/ac3rf-decoding.md](docs/ac3rf-decoding.md)** for a detailed explanation of how AC3-RF
decoding works.

### fl2kmuse

Standalone test signal generator for MUSE signals via an FL2K USB device.
This project has very limited scope and was used to validate from assumptions
about MUSE encoding.

### picostream

Standalone C wrapper for streaming data from a Picoscope 5000-series oscilloscope to a file or FIFO.
Used to capture MUSE RF in real time for piping directly into museld.
(The current tool I use to capture MUSE RF is [fx3usbadc](https://bitbucket.org/staffanulfberg/fx3usbadc).)

## Building

All components require CMake 3.22+, a C++23 compiler, and the vendored firpm submodule.

```bash
git submodule update --init --recursive
```

### museld + ac3rf-efm-decode + Python bindings (default)

All three are built by default. Dependencies: Vulkan, GLFW, PortAudio, GNURadio, FLAC++, Eigen3,
Python 3.8+, pkg-config.

Ubuntu packages:
```bash
sudo apt install cmake glslc libglfw3-dev portaudio19-dev libavformat-dev libavcodec-dev \
    libswscale-dev catch2 vulkan-tools vulkan-validationlayers-dev gnuradio-dev \
    libflac++-dev libeigen3-dev python3-dev
```

macOS (Homebrew): packages have similar names; use Homebrew for dependencies.

```bash
cmake -DCMAKE_BUILD_TYPE=Release -S player -B player/build-release
cmake --build player/build-release
```

GLSL shaders in `player/src/shaders/` are compiled to SPIR-V by `glslc` at build time.
nanobind (for the Python bindings) is fetched automatically by CMake.

### ac3rf-efm-decode only (minimal dependencies)

Dependencies: FLAC++ (`libflac++`), Eigen3, pkg-config.

```bash
cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_MUSE=OFF -DBUILD_PYTHON=OFF \
    -S player -B player/build-ac3rf
cmake --build player/build-ac3rf

# With AddressSanitizer
cmake -DCMAKE_BUILD_TYPE=Debug -DBUILD_MUSE=OFF -DBUILD_PYTHON=OFF \
    -S player -B player/build-debug
cmake --build player/build-debug
```

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
cmake -DCMAKE_BUILD_TYPE=Debug -DBUILD_MUSE=OFF -DBUILD_PYTHON=OFF \
    -S player -B player/build-debug
cmake --build player/build-debug
cd player/build-debug && ctest -V
```

## Quick start — museld

```console
wget --directory-prefix ../data/muse https://madeye.org/muse-demo/makeup-muse-rf-62.5MHz-nofilter.raw
./player/build-muse/src/musecpp --demodulate ../data/muse/makeup-muse-rf-62.5MHz-nofilter.raw
```

![MUSE test picture](test-picture.png)

## Author and License

Software written by Staffan Ulfberg 2021–2026.

Licensed under the [GNU General Public License v3](gpl-3.0.txt).
