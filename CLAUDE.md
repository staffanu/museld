# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Project Does

**museld** is a real-time MUSE/NTSC laserdisc player and audio decoder suite. Primary components:

- **museld** — Real-time MUSE (Hi-Vision HD) and NTSC laserdisc decoder with Vulkan GPU compute, GLFW display, and miniaudio audio output
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
    ocr/           — Live subtitle OCR + translation (PP-OCR on ONNX Runtime, no OpenCV):
                     OcrEngine, OcrWorker thread, Vulkan band readback, TranslationWorker
                     (OpenAI-compatible HTTP, cpp-httplib + nlohmann/json).  Built with
                     -DUSE_OCR=ON; museld --ocr <models-dir> adds a live "OCR" track,
                     --ocr-translate <url> adds "OCR-EN", --ocr-write saves both as .srt
                     with original imprint timing.  subocr-test is the regression harness
                     against the tools/subocr Python reference
    util/          — PercentileFilter, LinearRegression, ConstExprHelpers, FmtAddons
    shaders/       — GLSL compute shaders (compiled to SPIR-V at build time)
    museld.cpp
    ac3rf-efm-decode.cpp
  tests/           — Catch2 unit tests (ReedSolomonTest, BchDecoderTest, SrtParserTest)
  third_party/     — Vendored single-header libs (stb_truetype.h, miniaudio.h) and the bundled subtitle font (Noto Sans JP, SIL OFL)
  cmake/           — CMake helpers (ac3rfConfig.cmake.in, modules/FindLIBAV.cmake, etc.)
fl2kmuse/          — Standalone: MUSE test signal generator via FL2K USB device
picostream/        — Standalone: Picoscope oscilloscope capture tool (analog + MSO digital)
ldconv/            — Standalone: converter between capture formats (.lds/.ldf/.s16/...)
octave/            — Octave/Matlab scripts for filter design and algorithm exploration
tools/             — Miscellaneous tools (efm-filters, muse-de-emphasis, parseQ.awk,
                     srt-furigana.py for beginner-friendly Japanese subtitle variants,
                     subocr/ prototype: OCR burned-in Japanese subtitles from rendered
                     frames to .srt and translate them via the Claude API or a local
                     OpenAI-compatible model server)
packaging/         — Scripts and per-package READMEs for the Windows/macOS downloads
docs/              — Reference documentation (player, CLI, AC3-RF decoding, packaging)
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
# Release build (museld + ac3rf-efm-decode)
cmake -DCMAKE_BUILD_TYPE=Release -S player -B player/build-release
cmake --build player/build-release

# ac3rf-efm-decode only (minimal deps)
cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_MUSE=OFF -S player -B player/build-ac3rf
cmake --build player/build-ac3rf

# Debug build (includes AddressSanitizer)
cmake -DCMAKE_BUILD_TYPE=Debug -DBUILD_MUSE=OFF -S player -B player/build-debug
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

### ldconv (standalone — requires libFLAC++)

```bash
cmake -DCMAKE_BUILD_TYPE=Release -S ldconv -B ldconv/build-release
cmake --build ldconv/build-release
ldconv/tests/roundtrip.sh ldconv/build-release/ldconv   # lossless round-trip checks
```

Converts between `.lds`, `.ldf`, `.s16` and the other capture formats, replacing
`ld-compress` and `ld-lds-converter`. The conversions are byte-exact against those
tools; `ldconv/README.md` records the format conventions and the measured speeds.
The one exception is `--bits N`, which rounds samples to N bits of resolution to
make a capture compress smaller (`--info` reports the resolution a file uses).

## Running Tests

```bash
cmake -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -S player -B player/build-debug
cmake --build player/build-debug
cd player/build-debug && ctest -V
```

Tests are only built when `-DBUILD_TESTING=ON` is passed. They are in `player/tests/`
(`ReedSolomonTest.cpp`, `BchDecoderTest.cpp`, `FilterSimdParityTest.cpp`, `SrtParserTest.cpp`).

## CI and Packaging

`.github/workflows/ci.yml` builds on Linux, macOS and Windows (MSYS2 UCRT64) on every push,
and also packages downloadable binaries: three zips per platform (player, player with
FFmpeg for `--write`, and `ac3rf-efm-decode` alone), attached to a GitHub release when a
`v*` tag is pushed. The staging scripts live in `packaging/`.

Two options exist for the packages and matter when touching the build:

- `MUSELD_ARCH` — the value passed to `-march=`, default `native`. The packages set a
  portable baseline instead, since `native` targets the CPU doing the build. Empty means
  no `-march` at all (used for arm64 macOS).
- `USE_LIBAV` — default ON; OFF builds museld without the video file writer, which is how
  the small packages avoid FFmpeg.

See **docs/packaging.md** before changing any of this — it records why the packages are
split, what museld loads at runtime, and how Vulkan is wired up on macOS.

## Architecture

### src/ Internal Structure

The `src/` directory is the include root for both binaries. The `ac3rf` CMake target covers the reusable library (`ac3/`, `efm/`, `filter/`, `rs/`, `logging/`). The `museld` target adds everything else (`muse/`, `ntsc/`, `musevk/`, `bch/`, `util/`, `shaders/`).

### Key Design Patterns

**Static library / CLI separation**: `ac3rf` (pure decoding logic) is a library; `ac3rf-efm-decode` adds I/O.

**`ByteWithErasureFlag<T>` template**: Carries erasure metadata through the entire pipeline — filter stages, demodulation, Reed-Solomon — enabling fine-grained error tracking.

**Pluggable erasure concealment**: Abstract `ErasureConcealer` interface with four implementations (`RepeatingSample`, `LinearInterpolation`, `Ar`, `SlowAr`), selected via CLI.

**Input format abstraction**: `InputReader` specializations with auto-detection by file extension.

**SIMD-aware FIR filtering**: `FirFilterStage.h` has AVX, NEON, and scalar paths. On x86 the AVX path is compiled in regardless of `-march` (function-level target attribute) and chosen at runtime via CPUID, so `MUSELD_ARCH` sets which CPUs can run the binary without deciding whether AVX is used. `--simd`/`--no-simd` override the automatic choice; forcing `--simd` on a CPU without support is an error.

**Video file output presets**: `museld --write <file> --write-preset standard|archival` (`VideoFileWriter`, `VideoWriterOptions.h`). `standard` = H.264 (libx264, CRF) + AAC in MP4; `archival` = lossless FFV1 16-bit + PCM in Matroska. Container/codecs come from the preset, not the filename extension. Color metadata (BT.709 for MUSE, SMPTE 170M for NTSC, full range) is tagged per input type. Audio is kept in sync with the video clock: small deficits (25–100 ms) are concealed by stretching/interpolation, larger gaps by silence with a fade-out, excess audio by dropping samples. For batch rendering add `--no-sync`, otherwise the display loop caps decoding at 1.0x realtime. Raw captures without a recognized extension need `--input-format` (e.g. `s16`).

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
HD video + miniaudio
```

**NTSC path:**
```
RF (40 MHz) → NtscRfDemodulator → NtscInputReader DPLL →
NTSC frame buffer → Vulkan GPU color decode → video output
```

### Object Lifetimes and Teardown (museld)

Every class that owns a thread or an armed callback stops and joins it in its
destructor (`FrameReader` and subclasses, `RfDemodulator` and subclasses, the
EFM worker inside `demodulate()`, `AudioPlayback`, `OcrWorker`,
`TranslationWorker`). The `cleanup()` methods still exist for the explicit
happy-path teardown order but are idempotent, so destructor-driven unwinding
after an exception is always safe. Conventions to preserve when touching this
code: thread members are declared last in their class (joined before the state
they use is destroyed); worker-thread exceptions are marshalled via
`exception_ptr` and rethrown on the consumer's thread (`getNextInputBuffer`
ultimately rethrows on the main thread — never `std::exit` from a worker);
frame readers call `RfDemodulator::requestStop()` before joining their reader
thread (deadlock otherwise); destructors never throw. `process_file` tears
down via a RAII guard: reader → Vulkan → GLFW window, in that order.

### museld Threading

- Main thread: Vulkan command recording + GLFW event loop
- Worker thread: Resampling DPLL (CPU bottleneck)
- Demodulator thread (`museld-demod`): input read + RF demod GPU pipeline; logs per-section timing at info level (`ePerformance`)
- EFM worker thread (`museld-efm`): EFM demodulation off the demodulator thread; blocks flow vacant → demod → EFM queue → filled, order preserved by the single FIFO worker
- GPU: Async compute via SPIR-V shaders (`player/src/shaders/*.comp`)

### Compiler Flags

- Always: `-ffast-math`, and `-march=${MUSELD_ARCH}` — `native` by default, but the
  packages pin a portable baseline (see **CI and Packaging**); empty means no `-march`
- Debug: `-fsanitize=address`
- macOS: `-mmacosx-version-min=15.0`

### Reed-Solomon Decoder Strategies

`rs/ReedSolomon.h` supports four `DecodingStrategy` values:
- `RS_NONE` — no correction
- `RS_C1` — conservative: correct 1 error or 2 erasures, otherwise erase all
- `RS_C2` — corrects 1-3 erasures or 1 error (+1 erasure via two-error path)
- `RS_MAX` — error-only correction (erasure path currently disabled via `false &&`)

The CIRC pipeline uses RS_C1 for the first pass and RS_C2 for the second.
