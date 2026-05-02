# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Project Does

**ldaudio** is a suite of tools for decoding audio signals from laserdiscs. It supports:
- **EFM** (Eight-to-Fourteen Modulation): CD audio decoding with CIRC Reed-Solomon error correction
- **AC3RF**: QPSK-demodulated AC3 surround audio from laserdiscs
- **MUSE**: Hi-Vision laserdisc high-definition video/audio decoding

## Repository Structure

- `ac3rf-decode/` — C++23 static library + CLI for AC3RF and EFM decoding (primary production component)
- `musecpp/` — Real-time MUSE decoder with Vulkan GPU compute, GLFW display, and PortAudio
- `fl2kmuse/` — Test signal generator via FL2K USB device
- `picostream/` — C wrapper for Picoscope oscilloscope capture
- `scala/` — Reference Scala 3 implementations used for algorithm development and VHDL table generation
- `src/`, `wcfg/`, `project.tcp/` — VHDL Vivado projects (Xilinx Artix-7, Lattice iCE40)
- `ac3-efm-pmod/` — KiCad PCB design for analog signal conditioning

## Build Commands

All C++ components use CMake 3.22+ with out-of-source builds.

### ac3rf-decode

```bash
# First time: initialize vendored firpm submodule
git submodule update --init --recursive

# Release build
cmake -DCMAKE_BUILD_TYPE=Release -B build-release .
cmake --build build-release

# Debug build (includes AddressSanitizer)
cmake -DCMAKE_BUILD_TYPE=Debug -B build-debug .
cmake --build build-debug

# Optional Python bindings
cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_PYTHON=ON -B build-release .
```

### musecpp

```bash
cmake -DCMAKE_BUILD_TYPE=Release -B build-release .
cmake --build build-release
```

GLSL shaders in `shaders/` are compiled to SPIR-V via `glslc` as part of the build.

### Scala

```bash
cd scala && sbt compile
```

## Running Tests

### C++ (musecpp only — Catch2)

```bash
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug .
cmake --build build-debug
cd build-debug && ctest -V
```

Tests are in `musecpp/Catch_tests/` (`ReedSolomonTest.cpp`, `BchDecoderTest.cpp`).

### Scala (ScalaCheck property-based tests)

```bash
cd scala && sbt test
```

## Architecture

### ac3rf-decode Internal Structure

- `ac3/` — AC3-RF QPSK demodulation, DPLL, frame sync, decoder
- `efm/` — EFM demodulation, Mueller-Müller timing recovery, CIRC decoding, erasure concealment
- `filter/` — FIR/IIR primitives, Parks-McClellan design, FFT, SIMD-optimized (AVX/NEON)
- `rs/` — Header-only template Reed-Solomon codec over GF(2^8) with erasure support
- `input/` — Input format readers: raw uint8/16/sint8/16, `.lds` (10-bit packed), `.ldf` (FLAC-in-Ogg)
- `external/firpm/` — Vendored Parks-McClellan FIR design library (git submodule)

### Key Design Patterns

**Static library / CLI separation**: `ac3rf` (pure decoding logic) is a library; `ac3rf-decode` adds I/O. Python bindings wrap only the library.

**`ByteWithErasureFlag<T>` template**: Carries erasure metadata through the entire pipeline — filter stages, demodulation, Reed-Solomon — enabling fine-grained error tracking.

**Pluggable erasure concealment**: Abstract `ErasureConcealer` interface with four implementations (`RepeatingSample`, `LinearInterpolation`, `Ar`, `SlowAr`), selected via CLI.

**Input format abstraction**: `InputReader` specializations with auto-detection by file extension.

**SIMD-aware FIR filtering**: `FirFilterStage.h` has AVX, NEON, and scalar paths with runtime selection via `--simd`/`--no-simd`.

### Data Flow

**AC3RF path:**
```
Raw RF → bandpass FIR → decimation → IQ mixing (DPLL) →
post-mix decimation → QPSK demod → frame sync → de-interleave →
Reed-Solomon C1/C2 → AC3 output
```

**EFM path:**
```
RF/baseband → decimation filters → IIR lowpass → Mueller-Müller timing →
fractional resampler → EFM demod → CIRC C1/C2 decoding →
erasure concealment → pop detection → stereo PCM
```

**MUSE path (musecpp):**
```
RF (62.5 MHz) → RF demod → resampling DPLL (16.2 MHz) →
MUSE frame buffer → video/audio split → Vulkan GPU filters →
HD video + audio
```

### musecpp Threading

- Main thread: Vulkan command recording + GLFW event loop
- Worker thread: Resampling DPLL (CPU bottleneck)
- GPU: Async compute via SPIR-V shaders (`shaders/*.comp`)

### Compiler Flags

- Always: `-ffast-math`, `-march=native`
- Debug: `-fsanitize=address`
- macOS: `-mmacosx-version-min=15.0`

### Scala Reference Implementations

The `scala/` directory contains the authoritative algorithmic reference for EFM, AC3RF, and MUSE decoding. When implementing or verifying C++ behavior, consult the Scala versions first. The Scala code also generates GF(256) lookup tables used in the VHDL designs.
