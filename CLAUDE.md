# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Project Does

**museld** is a real-time MUSE/NTSC laserdisc player and audio decoder suite. Primary components:

- **museld** — Real-time MUSE (Hi-Vision HD) and NTSC laserdisc decoder with Vulkan GPU compute, GLFW display, and PortAudio audio output
- **ac3rf-efm-decode** — CLI tool (and reusable library) for AC3-RF QPSK surround audio and EFM CD audio decoding from laserdisc RF captures

Supported formats:
- **MUSE**: Hi-Vision laserdisc high-definition video + audio
- **NTSC**: Standard definition laserdisc video + audio
- **EFM**: CD audio (CIRC Reed-Solomon, stereo PCM)
- **AC3RF**: QPSK-demodulated AC3 surround audio

## Repository Structure

```
player/            — Main C++ project (museld player + ac3rf-efm-decode library/CLI)
  CMakeLists.txt
  src/             — All C++ source (shared between both binaries)
    ac3/           — AC3-RF QPSK demodulation, DPLL, frame sync, decoder
    efm/           — EFM demodulation, timing recovery, CIRC decoding, concealment
    filter/        — FIR/IIR primitives, Parks-McClellan, FFT, SIMD (AVX/NEON)
    rs/            — Header-only Reed-Solomon codec over GF(2^8) with erasure support
    input/         — Input format readers for ac3rf-efm-decode (lds, ldf/FLAC)
    logging/       — Abstract Logger, StreamLogger (shared by both binaries)
    bch/           — BCH decoder (MUSE control data)
    muse/          — MUSE frame buffers, audio/video decoders, GPU shaders interface
    ntsc/          — NTSC frame buffers, sync detection, field decoder
    musevk/        — Vulkan abstraction layer (buffers, images, command pools, compute)
    subtitles/     — SRT parser, stb_truetype-based glyph atlas, GPU subtitle overlay
    util/          — PercentileFilter, LinearRegression, ConstExprHelpers, FmtAddons
    shaders/       — GLSL compute shaders (compiled to SPIR-V at build time)
    museld.cpp
    ac3rf-efm-decode.cpp
  tests/           — Catch2 unit tests (ReedSolomonTest, BchDecoderTest, SrtParserTest)
  third_party/     — Vendored single-header libs (stb_truetype.h) and the bundled subtitle font (Noto Sans JP, SIL OFL)
  cmake/           — CMake helpers (ac3rfConfig.cmake.in, modules/FindPORTAUDIO.cmake, etc.)
  python/          — nanobind Python bindings source
fl2kmuse/          — Standalone: MUSE test signal generator via FL2K USB device
picostream/        — Standalone: C wrapper for Picoscope oscilloscope capture
octave/            — Octave/Matlab scripts for filter design and algorithm exploration
tools/             — Miscellaneous tools (efm-filters, muse-de-emphasis, parseQ.awk)
```

## Clangd / LSP Diagnostics

CMake generates `compile_commands.json` in the build directory, which clangd uses for accurate diagnostics. The user maintains a symlink `compile_commands.json` → `player/cmake-build-relwithdebinfo/compile_commands.json` at the repo root, so clangd reads from the CLion-managed RelWithDebInfo build.

Diagnostic errors about missing headers (Vulkan, GLFW, etc.) indicate the build directory hasn't been set up yet — they are not real code errors.

## Build Commands

All C++ components use CMake 3.22+ with out-of-source builds.

### Preferred build (RelWithDebInfo, used day-to-day)

The user normally builds and runs from `player/cmake-build-relwithdebinfo` (set up automatically by CLion). Default to this directory when building or testing changes — it's already configured with `BUILD_MUSE=ON`, so the `museld` target is available and incremental builds are fastest:

```bash
cmake --build player/cmake-build-relwithdebinfo --target museld
# or, to build everything:
cmake --build player/cmake-build-relwithdebinfo
```

### Other build configurations

```bash
# Release build (museld + ac3rf-efm-decode + Python bindings)
cmake -DCMAKE_BUILD_TYPE=Release -S player -B player/build-release
cmake --build player/build-release

# ac3rf-efm-decode only (minimal deps)
cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_MUSE=OFF -DBUILD_PYTHON=OFF -S player -B player/build-ac3rf
cmake --build player/build-ac3rf

# Debug build (includes AddressSanitizer)
cmake -DCMAKE_BUILD_TYPE=Debug -DBUILD_MUSE=OFF -DBUILD_PYTHON=OFF -S player -B player/build-debug
cmake --build player/build-debug
```

GLSL shaders in `player/src/shaders/` are compiled to SPIR-V via `glslc` as part of the build.

### fl2kmuse (standalone — requires libosmo-fl2k)

```bash
cmake -DCMAKE_BUILD_TYPE=Release -S fl2kmuse -B fl2kmuse/build-release
cmake --build fl2kmuse/build-release
```

### picostream (standalone — requires Picoscope SDK at /opt/picoscope)

```bash
cmake -DCMAKE_BUILD_TYPE=Release -S picostream -B picostream/build-release
cmake --build picostream/build-release
```

## Running Tests

```bash
cmake -DCMAKE_BUILD_TYPE=Debug -S player -B player/build-debug
cmake --build player/build-debug
cd player/build-debug && ctest -V
```

Tests are in `player/tests/` (`ReedSolomonTest.cpp`, `BchDecoderTest.cpp`).

## Architecture

### src/ Internal Structure

The `src/` directory is the include root for both binaries. The `ac3rf` CMake target covers the reusable library (`ac3/`, `efm/`, `filter/`, `rs/`, `logging/`). The `museld` target adds everything else (`muse/`, `ntsc/`, `musevk/`, `bch/`, `util/`, `shaders/`).

### Key Design Patterns

**Static library / CLI separation**: `ac3rf` (pure decoding logic) is a library; `ac3rf-efm-decode` adds I/O. Python bindings wrap only the library.

**`ByteWithErasureFlag<T>` template**: Carries erasure metadata through the entire pipeline — filter stages, demodulation, Reed-Solomon — enabling fine-grained error tracking.

**Pluggable erasure concealment**: Abstract `ErasureConcealer` interface with four implementations (`RepeatingSample`, `LinearInterpolation`, `Ar`, `SlowAr`), selected via CLI.

**Input format abstraction**: `InputReader` specializations with auto-detection by file extension.

**SIMD-aware FIR filtering**: `FirFilterStage.h` has AVX, NEON, and scalar paths with runtime selection via `--simd`/`--no-simd`.

**Video file output presets**: `museld --write <file> --write-preset standard|archival` (`VideoFileWriter`, `VideoWriterOptions.h`). `standard` = H.264 (libx264, CRF) + AAC in MP4; `archival` = lossless FFV1 16-bit + PCM in Matroska. Container/codecs come from the preset, not the filename extension. Color metadata (BT.709 for MUSE, SMPTE 170M for NTSC, full range) is tagged per input type. For batch rendering add `--no-sync`, otherwise the display loop caps decoding at 1.0x realtime. Raw captures without a recognized extension need `--input-format` (e.g. `s16`).

### Data Flow

**AC3RF path:**
```
Raw RF → bandpass FIR → decimation → IQ mixing (DPLL) →
post-mix decimation → QPSK demod → frame sync → de-interleave →
Reed-Solomon C1/C2 → AC3 output
```

**EFM path:**
```
RF/baseband → decimation filters → IIR lowpass → TimingRecovery (Mueller-Müller) →
fractional resampler → EfmDecoder → CIRC C1/C2 → concealment → pop detection → PCM
```

**MUSE path:**
```
RF (62.5 MHz) → RF demod → ResamplingInputReader DPLL (16.2 MHz) →
MUSE frame buffer → video/audio split → Vulkan GPU filters →
HD video + PortAudio
```

**NTSC path:**
```
RF (40 MHz) → NtscRfDemodulator → NtscInputReader DPLL →
NTSC frame buffer → Vulkan GPU color decode → video output
```

### museld Threading

- Main thread: Vulkan command recording + GLFW event loop
- Worker thread: Resampling DPLL (CPU bottleneck)
- Demodulator thread (`museld-demod`): input read + RF demod GPU pipeline; logs per-section timing at info level (`ePerformance`)
- EFM worker thread (`museld-efm`): EFM demodulation off the demodulator thread; blocks flow vacant → demod → EFM queue → filled, order preserved by the single FIFO worker
- GPU: Async compute via SPIR-V shaders (`player/src/shaders/*.comp`)

### Compiler Flags

- Always: `-ffast-math`, `-march=native`
- Debug: `-fsanitize=address`
- macOS: `-mmacosx-version-min=15.0`

### Reed-Solomon Decoder Strategies

`rs/ReedSolomon.h` supports four `DecodingStrategy` values:
- `RS_NONE` — no correction
- `RS_C1` — conservative: correct 1 error or 2 erasures, otherwise erase all
- `RS_C2` — corrects 1-3 erasures or 1 error (+1 erasure via two-error path)
- `RS_MAX` — error-only correction (erasure path currently disabled via `false &&`)

The CIRC pipeline uses RS_C1 for the first pass and RS_C2 for the second.
