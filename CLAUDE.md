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
- **Analog**: NTSC analog FM stereo audio (2.3011/2.8125 MHz carriers, CX expansion, squelch)

The SD pipeline (`sdtv/`) is parameterized by a `VideoStandard` (`sdtv/VideoStandard.h`:
geometry of the line-locked frame buffer, frame rate, RF carrier, VBI lines, noise windows;
`VideoStandard::ntsc()` and `::pal()`), and the `shaders/sdtv/sdtv_*.comp` sources are compiled
once per standard, as `ntsc_*.spv` and as `pal_*.spv` with `-DSDTV_PAL` (the `SDTV_*` macros in
`shaders/muse/muse.h` take the PAL values there).  Names: `Sdtv…` is what both standards share
(`SdtvFrameReader`, `SdtvDecoder`, ...), `Ntsc…`/`Pal…` only what belongs to one of them
(`NtscCadenceTracker`).  `--input-type pal-rf` is work in progress:
picture (colour: a 3-line comb over the ±90° neighbours, a line-pair burst reference for the
swinging burst and V switch, the delay-line U/V average and a temporal comb over frames N ± 2
-- the decoder keeps 3d + 1 frames with d = 2 on PAL and shows frame N two reads behind),
VBI, EFM audio, `--write` and content detection work; calibrated on the GGV1011 test disc.  `docs/pal-playback-plan.md` has the plan and the status.

NTSC playback in museld selects between the analog, EFM and AC3-RF tracks (`AudioTrack`,
A key / `--efm` / `--ac3`); a DTS bitstream on the EFM track is auto-detected. AC3 and DTS
are decoded by `CompressedAudioDecoder` (libavcodec + libswresample), which is gated on
`HAVE_LIBAV` — in builds without FFmpeg those tracks are selectable but silent. `AudioFrame`
carries up to 6 channels (5.1 slot order FL FR FC LFE SL SR); playback opens a 6-channel
device for the 5.1 modes. `--write` with the AC3 track muxes the original AC3 bitstream
(stream copy via `DecodedField::ac3_frames`); DTS and the other tracks are written as PCM,
with 5.1 downmixed to the file's stereo track.

## Repository Structure

```
player/            — Main C++ project (museld player + ac3rf-efm-decode library/CLI)
  CMakeLists.txt
  src/             — All C++ source (shared between both binaries)
    ac3/           — AC3-RF QPSK demodulation, DPLL, frame sync, decoder
    analog/        — Analog FM audio demodulator (NTSC carriers) and CX expander
    efm/           — EFM demodulation, timing recovery, CIRC decoding, concealment
    filter/        — FIR/IIR primitives, Parks-McClellan, FFT, SIMD (AVX/NEON)
    rs/            — Header-only Reed-Solomon codec over GF(2^8) with erasure support
    input/         — Input format readers for ac3rf-efm-decode (lds, ldf/FLAC)
    logging/       — Abstract Logger, StreamLogger (shared by both binaries)
    bch/           — BCH decoder (MUSE control data)
    muse/          — MUSE frame buffers, audio/video decoders, GPU shaders interface
    sdtv/          — Standard definition (NTSC and PAL) frame buffers, sync detection, field
                     decoder, the video standard descriptor
    musevk/        — Vulkan abstraction layer (buffers, images, command pools, compute)
    subtitles/     — SRT parser, stb_truetype-based glyph atlas, GPU subtitle overlay,
                     EIA-608 closed caption decoder (live "CC" track on NTSC discs,
                     line 21 sliced in sdtv/SdtvFrame; --cc-write saves <input>.CC.srt)
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
  third_party/     — Vendored single-header libs (stb_truetype.h, miniaudio.h, httplib.h), the ethadc
                     stream receiver headers (ethadc/, copied verbatim by its update.sh from ../ethadc;
                     included as "ethadc/X.h" -- never put a third_party subdirectory holding
                     non-header files on the include path, macOS/Windows ignore case)
                     and the bundled subtitle font (Noto Sans JP, SIL OFL)
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
(`ReedSolomonTest.cpp`, `BchDecoderTest.cpp`, `FilterSimdParityTest.cpp`, `InputProbeTest.cpp`,
`NtscCadenceTrackerTest.cpp`, `PrefetchingInputReaderTest.cpp`, `SrtParserTest.cpp`,
`DisplayGeometryTest.cpp`). On macOS
the Homebrew Catch2 is not ASan-instrumented, so run them with
`ASAN_OPTIONS=detect_container_overflow=0` to avoid a false container-overflow abort at startup.

## Debugging the NTSC decode

Judge sync and level quality from measurements, never from single frames or
downscaled montages (they lie — clean stretches land in the sample). The
instruments, all in the RelWithDebInfo build:

**Log categories** (`--log <letter><level>…`, letters MPAVDIO = Main, Performance,
Audio, Video, Decoder, Input, Output; levels 0–4 = off, error, warn, info, debug):
- `P3` — per-stage timing (demodulator per block, reader and decoder per frame) and
  the final frames/s; the frame rate is set by the slowest stage, the demodulator.
- `I3` — timebase: anchoring, resets, lattice slips, re-anchors. `I4` adds one line
  per frame, `N of M lines unsupported by a nearby sync pulse`; exactly 18 per
  frame (the two vertical intervals) means every hsync was found.
- `D3` — every 30 frames: noise/SNR, levels (blanking, white flag, gain), the black
  level statistic (`picture black: dark peak … held minimum …`, with the automatic
  NTSC-M/J choice), film cadence lock/unlock events with their reason.
- `D4` adds per frame: `VBI frame N: codes …; clv/time/pic/chapter/cx`, the VBI
  slicer's failure reasons per line, and `film cadence: frame N d0 d1 floor locked
  misses scores`.
- Options must precede the input file they apply to; trailing options are ignored
  with a warning.

**Dumps** (environment variables, append to the file; delete old dumps first):
- `MUSELD_DUMP_DEMOD=<path>` with `MUSELD_DUMP_DEMOD_BLOCKS=<n>` (default 32; a block
  is 262144 demodulated samples, 8.4 ms at 62.5 MHz input) — the demodulated
  composite as float32 at the video-decimated rate (31.25 MHz for 62.5 MHz captures,
  20 MHz for 40 MHz). Sync tip ≈ 0.0, blanking ≈ 0.3, white ≈ 1.0; no de-emphasis.
- `MUSELD_DUMP_TIMEBASE=<prefix>` — `<prefix>.curve.f64`: (line k, input sample
  position of its start) per finalized line; `<prefix>.meas.f64`: the accepted sync
  measurements. Use together with a demod dump **from the same run**.
- `MUSELD_DUMP_VBI_FAIL=<path>` — each VBI code line that failed to slice, as 910
  floats (the 4 fsc frame-buffer row).
- `MUSELD_DUMP_FRAME=<path>` — one whole frame buffer (the de-emphasized, rescaled
  composite, `total_lines × samples_per_line` float32, blanking 0, white 1) and the
  reader's raw line-locked composite of the same frame as `<path>.raw` and the dropout
  detector's flags as `<path>.do` (one byte per sample); the first frame after
  `MUSELD_DUMP_FRAME_NO` (default 30).

**Offline checks** (`tools/`, numpy + scipy + PIL): `ntsc-sync-delta.py <fs> <demod
dump> <timebase prefix> <out>` plots detected minus reconstructed sync per line —
must stay within ±0.1 µs with no walks; `ntsc-burst-check.py <prefix> <dump> <out>
<fs>` measures the curve's timing error against the colour burst phase, content-
independent — expect ±40 ns and ~1 ns line-to-line jitter; `pal-pilot-check.py <prefix>
<dump> <out> <fs>` is the PAL counterpart, against the 3.75 MHz pilot burst on the sync tip
(measured ±30 ns, 2 ns jitter on the GGV1011 test disc and the NYCSTM captures; the reader
refines PAL line starts with the pilot, and `<prefix>.pilot.f64` holds the corrected curve --
run the tool on it as a prefix to see the residual, 0.4 ns).

**Sample rates**: anything in the sync pass and timebase that is a count of (decimated)
samples must be derived from a time, never a constant: a 30 MHz Domesday Duplicator capture
(`~/Downloads/RF_Reference_30mhz.ddd.s16`, with a 40 MHz capture of the same disc beside it)
exposed two such constants in 2026-10 -- a back-porch window that reached into the picture at
15 MHz demod rate, and pulse widths in whole decimated samples (0.53 µs there) that let
equalizing pulses pass as hsync. Both showed as per-field sawtooth in `ntsc-burst-check.py`,
vertical-interval missing-runs shorter than 9 in the timebase dump, and `vertical drift`/
`lattice slipped` warnings. Re-run the 30, 40 and 62.5 MHz checks after touching that code.

**Frames**: `--seek T --export-frame-at T2 --export-frame f.png` writes the decoded
764×480 image (all of it: the display shows only its 4:3 middle, see `PictureFormat`; `V3`
logs the part shown and where) (`--field-interpolation intra-field` for a single field, `--no-3d-comb`,
`--no-film-mode`, `--black-level m|j` to isolate stages). `--export-frame-at` counts
displayed fields, so do not combine it with `--no-sync`. The GNOME session must be
unlocked or the PNGs come out black. Test captures on the development machine:
`~/alien2.ldf` (40 MHz, clean reference), `~/data/white-squall-*.s8` (62.5 MHz s8, noisy,
strong wow) and `~/data/godzilla*.s8` (62.5 MHz s8, an NTSC-J disc; the first file has a
cadence phase jump at ~25 s).

## CI and Packaging

`.github/workflows/ci.yml` builds on Linux, macOS and Windows (MSYS2 UCRT64) on every push,
and also packages downloadable binaries: two zips per platform (the player and the decoder
CLI, with and without FFmpeg for `--write` and ONNX Runtime for `--ocr`), attached to a
GitHub release when a `v*` tag is pushed. The staging scripts live in `packaging/`.

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

The `src/` directory is the include root for both binaries. The `ac3rf` CMake target covers the reusable library (`ac3/`, `analog/`, `efm/`, `filter/`, `rs/`, `logging/`). The `museld` target adds everything else (`muse/`, `sdtv/`, `musevk/`, `bch/`, `util/`, `shaders/`).

### Key Design Patterns

**Static library / CLI separation**: `ac3rf` (pure decoding logic) is a library; `ac3rf-efm-decode` adds I/O.

**`ByteWithErasureFlag<T>` template**: Carries erasure metadata through the entire pipeline — filter stages, demodulation, Reed-Solomon — enabling fine-grained error tracking.

**Pluggable erasure concealment**: Abstract `ErasureConcealer` interface with four implementations (`RepeatingSample`, `LinearInterpolation`, `Ar`, `SlowAr`), selected via CLI.

**Chapter search and picture stops**: the Up/Down keys and `--chapter` find a chapter start by
probing the input (`ChapterSearch`, pure decision logic with a Catch2 test, driven by the
player loop in `museld.cpp`). Probes use `DecodeControls::metadata_only` (first decoder stage
only, no picture or audio), absolute seeks (`FrameReader::seekToInputSample`, returning a seek
generation that every frame carries in `DecodedField::seek_generation`, so frames from before
the seek are told apart) and the input length (`InputReader::sampleCount()`, taken from the
file's tail for FLAC: the last Ogg page's granule position, or the last frame header of a
plain stream). STREAMINFO's sample count is only 36 bits (28 min at 40 MHz), so long captures
have it at zero or wrapped, and libFLAC refuses seeks beyond a wrapped value: `LdfInputReader`
zeroes the field in the bytes it hands libFLAC (re-checksumming the Ogg page), which makes it
seek by stream length instead. A seek the input refuses returns false up the chain
(`InputReader::seek`, `RfDemodulator::seekLocked`, `FrameReader::seek`) and nothing moves. Readers
reset their timebase/PLL on the reader thread when a block's generation changes, never from
the seeking thread, and the NTSC reader drops the partial frame after a reset. A probe costs
about 50 ms. `DiscInfo` exposes chapter, lead-in/out and the CAV picture stop (which pauses
playback unless turned off with P).

**Input format abstraction**: `InputReader` specializations (raw widths, lds, FLAC) with
auto-detection by file extension, reading through a `ByteSource` (`input/ByteSource.h`) that
hides where the bytes come from: a file or fifo (`FdByteSource`), a web server via HTTP range
requests (`HttpByteSource`, cpp-httplib, prefetching 3×4 MiB ahead) or the live ethadc UDP
capture stream (`EthadcByteSource`, built on the vendored `third_party/ethadc` receiver; not
in the Windows build). Names starting with `http://` or `udp://` select them; `inputIsLive()`
replaces the old `filesystem::is_fifo()` checks. A udp:// stream's format and rate come from its
packet headers (`probeNetworkStream`), and the ethadc `u10p` format is the `.lds` packing.

**Content-based input detection**: `src/InputProbe.{h,cpp}` (museld only, CPU-only, no GPU)
detects sample format, MUSE/NTSC RF type and sample rate from short chunks of the file.
This is museld's default: `--input-type` and `--sample-freq` are probed when not given
(so fifo input needs both), and `--probe` prints the measurements instead of decoding.

**No bare `long` for sample offsets or bit patterns**: Windows is LLP64, so `long` is 32
bits there (this once broke NTSC vertical sync lock on Windows only). Sample/file offsets
use `int64_t`, bit-pattern accumulators `uint64_t`, with `ULL` suffixes on wide literals.
File seeking goes through `seekFile()` (`input/FileSeek.h`), never bare `lseek`/`off_t`,
which are 32-bit on MinGW.

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
RF (62.5 MHz) → RF demod → ResamplingFrameReader DPLL (16.2 MHz) →
MUSE frame buffer → video/audio split → Vulkan GPU filters →
HD video + miniaudio
```

**NTSC path:**
```
RF (40 MHz) → SdtvRfDemodulator → SdtvFrameReader (feed-forward timebase: sync pulse
pass → line lattice → fixed-lag Kalman smoother → per-line resample, no DPLL) →
NTSC frame buffer (4 fsc, 910×525) → Vulkan GPU color decode → video output
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
- Reader thread: for MUSE the resampling DPLL (the CPU bottleneck); for NTSC the sync pass, timebase fit and per-line resampling (about 2 ms per frame)
- Demodulator thread (`museld-demod`): input read + RF demod GPU pipeline; logs per-section timing at info level (`ePerformance`), for both MUSE and NTSC
- EFM worker thread (`museld-efm`): EFM demodulation off the demodulator thread; blocks flow vacant → demod → EFM queue → filled, order preserved by the single FIFO worker. The NTSC worker (EFM or analog demod always runs), the NTSC reader thread (timebase and resampling vs input wait per frame) and the NTSC decoder (per-section, including the fence waits and the host reads of mapped buffers) also log `ePerformance` timing
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
