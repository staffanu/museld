# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Initialize submodules (required first time)
git submodule update --init --recursive

# Configure and build (Release)
cmake -DCMAKE_BUILD_TYPE=Release -B build-release .
cmake --build build-release

# Debug build (enables ASAN)
cmake -DCMAKE_BUILD_TYPE=Debug -B build-debug .
cmake --build build-debug

# Run tests
cd build-debug && ctest
```

Dependencies: Eigen3 (header-only), FLAC++ (libflac++), CMake 3.22+.

## Architecture

This is a C++23 signal processing application for decoding audio from laserdisc RF captures. It is structured as a **static library (libac3rf)** plus a **CLI application (ac3rf-decode)**.

There are two independent decoding pipelines:

**AC3 RF path:** Raw RF → pre-mix decimation (FIR) → IQ mixing & DPLL → post-mix decimation → QPSK demodulation → frame sync → Reed-Solomon → AC3 audio bursts. Key classes: `Ac3RfDemodulator`, `Ac3Decoder`, `Ac3DPLL`, `Ac3InputFraming`.

**EFM/CD path:** RF/baseband → decimation filters → IIR filtering → Mueller-Muller timing recovery → EFM demodulation → CIRC decoding (C1/C2 Reed-Solomon) → pop detection → erasure concealment → stereo PCM. Key classes: `EfmDemodulator`, `EfmDecoder`, `TimingRecovery`, `FractionalResampler`.

### Key directories

- `ac3/` — AC3-RF demodulation and digital decoding
- `efm/` — EFM (CD audio) demodulation, decoding, and concealment strategies (`efm/concealment/`)
- `filter/` — DSP primitives: FIR/IIR filters, filter design, FFT, windowing. SIMD-optimized (AVX/NEON with scalar fallback)
- `rs/` — Template-based Reed-Solomon codec over GF(2^8), with erasure support
- `input/` — Input format readers (raw uint8/16, sint8/16, ld-decode .lds 10-bit packed, .ldf FLAC-in-Ogg)
- `external/firpm/` — Vendored git submodule for Parks-McClellan FIR filter design

### Design patterns

- `ByteWithErasureFlag` carries erasure metadata through the entire pipeline
- Input formats are abstracted via `InputReader` with template specializations
- Erasure concealment is pluggable via `ErasureConcealer` factory (repeat, linear interpolation, AR model)
- FIR filter stages support cascaded decimation with configurable block sizes
- `Logger` is a pure interface; `StreamLogger` is the implementation
- The application outputs raw PCM to stdout for piping to ffplay/sox/ffmpeg
