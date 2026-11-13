# Porting museld to tvOS (Apple TV)

The goal: play captures on a box people already own.  An Apple TV has no USB, so its
inputs are the `http://` reader (a capture on a NAS) and the `udp://` reader (the live
[ethadc](https://github.com/staffanu/ethadc) stream, or `ethadc-tx` replaying a file);
see docs/museld.md, *Network input*.  Test device: an Apple TV 4K 2017 (A1842, A10X),
enough to find the bottlenecks; a 2021+ model (A12/A15) is the realistic target for
MUSE.  Development needs a Mac with Xcode; a free Apple ID suffices to run on your own
Apple TV (7-day signing, re-deploy weekly), the paid program only for TestFlight.

## Done

- **Network input** (master): `ByteSource` under the readers; HTTP range requests with
  prefetch, and the ethadc UDP stream embedded in-process through the vendored
  `player/third_party/ethadc` (never put that directory itself on the include path).
- **SDL3 in place of GLFW** (branch `sdl3`): window, Vulkan surface, keys, full screen.
  Frame exports are byte-identical to the GLFW build.  GLFW had no tvOS; SDL3 has.
- **The tvOS build** (branch `sdl3`, `player/CMakePresets.json`): presets `tvos-device`
  and `tvos-simulator`, Xcode generator, `CMAKE_SYSTEM_NAME=tvOS`, deployment target
  17.0, no `-march`, no FFmpeg/OCR/CLI.  Everything is built into the app or linked
  statically, fetched at configure time: SDL3 3.4.18 (the existing fallback), libogg
  1.3.6 + libFLAC 1.5.0 (`.ldf` files are *Ogg* FLAC, so `WITH_OGG` is needed; the
  `museld_find_flac()` macro in `src/CMakeLists.txt` picks pkg-config or this), the
  Khronos Vulkan headers 1.4.350, and MoltenVK 1.4.2's static xcframework from its
  GitHub release (172 MB `MoltenVK-all.tar`; `MUSELD_MOLTENVK_XCFRAMEWORK` points at
  another, e.g. the LunarG SDK's).  `AudioPlayback.cpp` compiles as Objective-C++ on
  tvOS (miniaudio's AVAudioSession code).  The bundle is flat: `shaders/` and `fonts/`
  land next to the executable, so `get_executable_dir()` needed no change.
- **Vulkan without a loader**: vulkan.hpp's static dispatcher binds straight to
  MoltenVK.  `VK_KHR_portability_enumeration` and its instance flag are the *loader's*
  and MoltenVK refuses them, hence `MUSELD_VULKAN_NO_LOADER` (tvOS only;
  `portability_subset` stays).  The surface is made from the window's Metal layer
  (`SDL_Metal_CreateView` + `vkCreateMetalSurfaceEXT`) on all Apple platforms, with
  `VK_USE_PLATFORM_METAL_EXT` defined for the museld target; macOS frame exports are
  unchanged by this.
- **App shell**: `SDL_main.h` on Apple (tvOS entry point), `SDL_INIT_GAMEPAD` with the
  Siri Remote / controller buttons mapped onto the keys in `InputController`
  (select, play/pause, Start → Space; Menu, Back → quit; touch surface/d-pad → arrows;
  Y → audio track; shoulders → subtitle slots), `WILL_ENTER_BACKGROUND` /
  `DID_ENTER_FOREGROUND` pausing the decoder and skipping all GPU work
  (`PlayerState::in_background`), `SDL_EVENT_TERMINATING` as quit.  SDL's UIKit runner
  keeps the process in its run loop after `main` returns, so `main` ends through
  `leave()`, which calls `exit()` on tvOS.  Info.plist template in
  `player/cmake/tvOS-Info.plist.in` (local network usage description, full screen).
  The Xcode scheme is generated; its launch arguments are museld's command line
  (`MUSELD_TVOS_LAUNCH_ARGUMENTS`, a `;`-separated list).  Signing is automatic with
  `MUSELD_DEVELOPMENT_TEAM`; both belong in `player/CMakeUserPresets.json`
  (git-ignored), inheriting the presets.
- **Simulator result** (tvOS 27 simulator on an M2): the app installs and runs, the
  content probe and the whole CPU side work (`--probe` agrees with the macOS build,
  the NTSC decoder logs sane SNR and levels at ~15 frames/s), MoltenVK creates the
  device and the swap chain -- but every GPU output is black, the OSD text included,
  with no MoltenVK error.  The simulator's Metal is an Apple 2 family GPU with no
  shared textures, which likely breaks the mapped-memory image uploads; it is not the
  hardware and this was not pursued.  Test on the device.

- **Runs on the Apple TV** (2026-10-17, the A1842 on tvOS 26.6): picture and sound,
  playing `.ldf` captures over HTTP from a Mac.  Signing is the free Apple ID's
  Personal Team (its ID differs from the one in the signing certificate; Xcode's
  Signing tab shows it), and the device is registered to the team by building the
  *scheme* with the Apple TV as destination once (`xcodebuild -scheme museld
  -destination id=<udid> -allowProvisioningUpdates -allowProvisioningDeviceRegistration`
  in the build directory); after that `cmake --build --preset my-tvos-device` signs.
  Install and run with `xcrun devicectl device install app --device <udid> <app>` and
  `xcrun devicectl device process launch --console --terminate-existing --device <udid>
  se.ulfberg.museld <options> <input>`.  Found on the device, all fixed:
  - the A10X allows 512 compute threads per workgroup, museld assumed 1024: the
    linear workgroup now halves down to `maxComputeWorkGroupInvocations`, and
    `envelope_prefix.comp` sizes its per-thread array from `gl_WorkGroupSize`;
  - miniaudio's default audio session category is PlayAndRecord, which tvOS refuses,
    and it fell back to its null device silently: `AudioPlayback` asks for the
    playback category and forwards miniaudio's log to the audio category;
  - a remote's select and play/pause buttons arrive as *Return* key presses (an
    HDMI-CEC remote for certain; the Siri Remote also when tvOS translates it rather
    than SDL seeing it as a gamepad), the directions as arrow keys, back as Escape, so
    Return pauses too.  No gamepad was seen with the CEC remote; the Siri Remote
    gamepad mapping is still untested.
- **Measured on the A1842.**  NTSC 40 MHz over HTTP: about 24 frames/s of 30, the
  demodulator block at 16 ms for a 13 ms budget, 13 of them waiting for the GPU
  (2 ms HTTP read + FLAC); the decoder 22 ms per frame.  A 62.5 MHz raw capture ran
  at 8.6 frames/s: the GPU chain's cost grows with the square of the sample rate (FIR
  taps and samples both follow it).  Two changes since: the tvOS window is 1080p
  rather than the 4K-density layer (the final blit cost 7-10 ms per frame there),
  and captures above 50 MHz are decimated by 2 before the band-pass
  (`SdtvRfDemodulator`, `pre_decimation`), the chain then running as for a 31.25 MHz
  capture with the video rate unchanged.  That took the 62.5 MHz excerpt to 19.9
  frames/s, the demodulator's GPU wait from 28 to 9.3 ms per 8.4 ms block, with the
  decoded SNR equal or slightly better on macOS and the 40 MHz path bit-identical.
  Per-shader GPU timestamps are useless under MoltenVK (one resolve at the end: the
  first shader gets the whole chain, the rest read the 37 us granularity).

## To do, in order

### 1. On the device

- Subtitles are seen on the device.  `render_subtitle.comp` was the one pipeline the
  A10X refused ("XPC_ERROR_CONNECTION_INTERRUPTED" from Metal's compiler service): a
  read-write rgba32f image, which Metal GPU family 3 only allows in 32-bit
  single-channel formats; it binds the frame twice now, read-only and write-only.
  Verified with the line-21 captions of the White Squall excerpt and `--cc`, which
  puts the CC track in the primary slot from the start (a TV remote has no `[` key).
  An .srt file reaches the app from the web server the capture is on: the tracks are
  the `.srt` files the index page of the capture's directory lists with the capture's
  name minus its extension (`subtitles/SubtitleDiscovery`), fetched over HTTP, and looked
  for without an option on every platform (the first is shown, and the disc's remembered
  choice, "off" included, overrides that; `--subtitles` is gone).  `--subtitles-file` takes an
  http:// address too.  Seen on the box 2026-11-11: the blue1 capture from the NAS with
  its en, ja and sv tracks beside it.  A file in the app's data container still cannot be read (put there
  with `devicectl device copy to`, the app's stat of it fails with "Operation not
  permitted"); a leading `~/` in file arguments is expanded (`expandHome`) for when
  that works.
- Speed: 40 MHz NTSC plays in real time with audio and margin.  Two steps after the
  pre-decimation: the FIR shader's outputs per invocation went to 1 on tvOS (on the
  A10X the fastest by far, the 40 MHz block's GPU time 14.7 -> 9.7 ms; the M2 wants
  2; `MUSELD_FIR_OUTPUTS=n` overrides for measuring, through `devicectl ...
  --environment-variables '{"MUSELD_FIR_OUTPUTS":"4"}'`), and the demodulator reads
  block N + 1 while the GPU works on block N (two input buffers alternating, the FIR
  history carried by the CPU).  Measured with audio logged (`--log A3`, which the
  earlier "no underruns" claims lacked): two minutes of the 40 MHz Donna Summer disc
  with 0 underruns and 0 discarded samples, the reader waiting 13 of its 33 ms per
  frame, the GPU 2.8 ms per block after the read.  Before the overlap it was 1.5%
  short of real time, an underrun about once a minute.  The 62.5 MHz raw excerpt is
  bound by the Mac's WiFi at ~20 frames/s, not by the GPU: measure it from a wired
  server, or as FLAC.  Next in line for MUSE: the analog audio demodulation on the
  CPU (7.6 of 13.1 ms per block at 40 MHz), and the decoder (17.5 of 33 ms per frame).
- **MUSE runs on the A1842** (2026-10-18, Ursa Minor Blue side 1 at 62.5 MHz from the
  NAS): at about 64% of real time -- 5740 fields in 150 s -- the demodulator's GPU at
  11.3 ms per 8.4 ms block (no pre-decimation here: the MUSE RF band needs the rate),
  the decoder about 37 ms per frame.  Measured apart (`--no-video --no-audio
  --no-sync`, and `MUSELD_REPEAT_INPUT=1` to run the GPU chain on one block over and
  over with no input reading at all): the MUSE demodulator's GPU chain is 8.4 ms per
  8.4 ms block, exactly the budget, and the decoder's GPU share about 3.4 ms per
  block, from the 73% of real time that raw s8 playback reaches.  Both demodulators
  read the next block while the GPU works on the current one (the same structure in
  `SdtvRfDemodulator` and `MuseRfDemodulator`).  Halving the band-pass and low-pass
  taps (`MUSELD_FIR_TRANSITION_SCALE=2`, 29 -> 15 complex taps) saves only ~0.6 ms of
  the 8.4, quartering ~1.5.  A fused complex band-pass (one pass producing the real
  and the imaginary output from one window load, in place of two fir_filter.comp
  passes) was *slower* on the A10X, 8.4 -> 11.1 ms for the chain, whether the taps sat
  in registers or were read from the buffer in the loop (8% faster on the M2, 1.15 ->
  1.06 ms; not kept).  So neither the taps nor the per-invocation cost of the FIRs is
  where the time goes: the tap-proportional part of the two FIRs is ~2 ms, and 5 ms or
  more of the 8.4 are the other passes (the quadrature demodulation with its atan, the
  envelope box, block sums and prefix scan, the dropout detector, the decimating
  low-pass and the RRC).  This GPU wants small simple kernels (one output per
  invocation won for the same reason).
- **The ablation** (`MUSELD_SKIP_PASS=name,...` leaves named passes out, with
  `MUSELD_REPEAT_INPUT=1`; the chain's 8.4 ms on the A10X by subtraction): band-pass
  (two passes) 4.0 ms, decimating low-pass 0.8, RRC 0.2, quadrature demodulation 0.4,
  the envelope box, block sums, prefix scan and dropout detector 1.4 together.  So the
  FIRs are 5.4 of the 8.4 after all, the band-pass alone half the chain, and the tap
  experiment misled: the cost is per output, weakly per tap.  Everything tried on the
  band-pass kernel was equal or worse: the fused complex pass (+2.7 ms), the input
  read from a device-local copy instead of the mapped buffer (no change), an array-free
  FIR reading samples and taps straight from the buffers (`fir_filter_direct.comp`,
  `MUSELD_FIR_SHADER=`; 5% better here, and the default on tvOS since the decimating
  band-pass below, whose 35 taps the blocked shader cannot hold), that FIR over
  sub-blocks (worse by the dispatch count), and more outputs per thread with it (2:
  level, 4: +25%, 8: 3x).  This GPU wants one output per thread and nothing else, and
  then does about 7.6 G tap-samples/s on a 29-tap FIR, which is what the band-pass's
  4 ms is.  Half floats are therefore not the lever either; fewer outputs are.
- **The decimating band-pass** (2026-10-18, done): the band-pass's output is analytic
  and 19.6 MHz wide (2.7 to 22.3 MHz), and a complex signal at 31.25 MHz holds any
  31.25 MHz wide band, so the band-pass decimates by 2 itself (the FIR shader's
  decimation constant, `analytic_decimation` in `MuseRfDemodulator`, 2 when fs/2 holds
  the band with its transitions, i.e. above about 43 MHz, 1 below;
  `MUSELD_ANALYTIC_DECIMATION=1` forces the old chain for A/B) and the quadrature
  demodulation, the envelope passes and the dropout detector run at the analytic
  rate, the low-pass no longer decimating.  Three things had to change with it, each
  found by comparing the demodulated 16.2 MHz baseband (`--write-muse16`, aligned by
  cross-correlation) and the decoder's noise statistics (`--log D3`: the clamp,
  level-239 and level-16 sigmas, the SNR after de-emphasis) against the old chain on
  blue1-fx3-62.5MHz.ldf:
  (1) `fm_quadrature.comp` took atan of sample * conj(prev) and subtracted the
  carrier's phase step afterwards, which wraps at 31.25 MHz (the carrier steps 2.51 rad
  per sample, the top of the band 4.48); it now rotates the product by
  e^{-j 2 pi fc / fs} (a constant from the caller, so no dependence on fc dividing fs)
  before the atan.  (2) The rectangular-window band-pass is only 32 dB down at the
  real input's negative-frequency image of the band, which folds across the band at
  31.25 MHz and beat with the carrier into the picture at 6.25 MHz: 11% rms difference
  in the baseband, all above 5 MHz.  A Kaiser design fixed that but let the EFM and
  pilot below 2.7 MHz through, which tripled the level-16 line's noise (they beat with
  the carrier at its low end into the top of the video band).  The band-pass is now a
  weighted least-squares design (`filter/LeastSquaresFir.h`): the image 50 dB and more
  down, the EFM region about what the rectangular design gave, the pass band flat
  within 0.1 dB, 43 complex taps at 62.5 MHz (35 since, see below).  (3) The discriminator's phase step
  over one analytic sample averages the instantaneous frequency over that interval, a
  sinc 1 dB down at 8 MHz at 31.25 MHz (0.25 dB at 62.5), measured as 0.9 dB lost at
  7-8 MHz; the low-pass is a least-squares design to the inverse sinc, 23 taps.
  Result: the baseband within +-0.4 dB of the old chain's across 0-9 MHz (the
  remainder its own rectangular ripple), SNR 33.8 dB in both, level-239 sigma 5.96
  vs 5.79, level-16 5.56 vs 5.08, SNR after de-emphasis 43.2 vs 43.3 dB.
  On the A10X the chain then took 19 ms per block with `fir_filter.comp`: the GPU
  cannot hold the 43-tap window and tap arrays in registers.  With the array-free
  `fir_filter_direct.comp`, now the default on tvOS (the M2 prefers the blocked one:
  1.3 vs 2.1 ms there), the chain is 6.4 ms per 8.4 ms block against 8.4 for the old
  chain with the same shader: band-pass 2.75, low-pass 0.8, quadrature 0.3, the rest
  (RRC, envelope passes, dropout detector, the copies and the dispatch overhead) 2.5.
  Playing the raw s8 from the NAS with audio: 9.6 ms per block total, 26.0 frames/s
  of 30 (was 73%), still 65 underruns in 100 s: the decoder's own GPU share (3.4 ms
  per block) and the demodulator together are 1.2 ms over the budget.  The FLAC
  capture from the NAS is no longer GPU-bound at all: the read (FLAC decoding on the
  reader's thread plus the HTTP wait, the NAS giving 60-100 MB/s against the 66 MB/s
  the file needs) takes 10.3 ms per block with the GPU idle 0.7 ms after it, 82% of
  real time, 236 underruns in 100 s.  Candidates for the GPU's 1.2 ms:
  a shorter band-pass (done, below), the decoder's GPU passes (unmeasured so far),
  the 1.6 ms of per-block overhead in the chain's eleven dispatches and copies.  For
  FLAC the lever is the input side: a local or faster source, or FLAC decoding off
  the reader thread.
- **The shorter band-pass** (2026-10-19, done): the least-squares specification had
  margin.  `MUSELD_BANDPASS_TAPS=n` sets the count at run time for A/B.  Designs: 43
  taps have 0.1 dB pass-band ripple and the image 50 dB and more down; 35 have 0.8 dB
  ripple, the image 43 to 57 dB down and the EFM region as before; at 31 the image
  rejection at DC is down to 29 dB.  The decoder's statistics on the blue1 capture
  (Mac) agree: 35 taps equal 43 (SNR 33.85 dB, sigma 6.01 at level 239 and 5.73 at
  16, 43.2 dB after de-emphasis), 31 lose the de-emphasis SNR (38.5 dB).  On the
  A10X in repeat mode the chain is 5.25 ms per block at 35 taps against 6.37 at 43
  (31: 5.62, 27: 5.07 -- the cost is not monotonic in the taps at this size).
  Playing the raw s8 from the NAS with audio, back to back: 9.21 ms per block
  (read 0.94, GPU wait 7.67), 27.1 frames/s, 45 underruns in 100 s, against 9.65 ms,
  25.8 frames/s and 67 underruns at 43 taps.  (One 35-tap run read 5.5 ms per block
  from the NAS and did worse, 21.9 frames/s; the NAS's speed varies run to run and
  the rerun read 0.94 ms.)  35 taps is the default.  The GPU is still about 0.8 ms
  over the budget per block with the decoder's 3.4 ms; what remains is the decoder's
  passes and the per-block dispatch overhead.
- **Packed integer input to the band-pass** (2026-10-19, tried, not kept): the
  band-pass read its input as the capture holds it, four s8 (or two s16) samples per
  word extracted with bitfieldExtract, a quarter of the float input's bytes.  A first
  measurement looked like 0.9 ms per block of GPU time saved, and was an accounting
  error: the demodulator's `gpu wait` is timed from the end of the next block's read,
  which runs while the GPU works, and the experiment's host-side re-quantising loop
  lengthened that read by the same 0.9 ms.  The GPU's time per block is read plus gpu
  wait when the read is the shorter, and that sum was equal (8.53 against 8.57 ms in
  real-time playback; the same on the M2).  Done properly -- the readers handing out
  integers through `InputReader::readNative`, the prefetching reader queueing them, one
  SPIR-V per sample format with the widths as glslc defines, the DC estimate taken out
  per output as dc times the tap sum -- it measured, A10X in repeat mode with no read
  to overlap, 5.27 ms per block against 4.86 for floats: the extraction costs ALU and
  the bytes were never the limit, as the ablation had said.  Real-time s8 playback:
  total 9.05 against 9.16 ms, the difference the reader's float conversion loop (read
  0.69 against 0.92 ms), which overlaps the GPU anyway.  Quality was identical.  Not
  worth the code; reverted, the diff kept outside the tree.  Lesson for the timing
  lines: compare `total`, or read plus gpu wait, never gpu wait alone when the read
  changes.  Half floats would be the same story: the FIR is arithmetic-bound here.
- **The decoder's passes, ablated** (2026-10-19, measured, nothing changed yet): the same
  `MUSELD_SKIP_PASS` hook on the MUSE decoder's shaders, real-time s8 playback with
  audio, 60 s each; the decoder's `Field N elapsed time` (both command buffers' fence
  waits, so its GPU time per field, which shares the GPU with the demodulator), the
  demodulator's total per block, and the frame rate:

  | skipped | decoder ms/field | demod ms/block | frames/s |
  |---|---|---|---|
  | nothing | 13.3 | 9.16 | 27.2 |
  | apply_equalizer | 12.7 | 8.84 | 28.2 |
  | apply_rescale_and_non_linear | 13.3 | 9.13 | 27.3 |
  | apply_dropout_compensation | 13.3 | 9.16 | 27.3 |
  | apply_deemphasis_and_gamma | 13.3 | 9.13 | 27.4 |
  | copy_y_for_interpolation (5 dispatches) | 12.9 | 8.97 | 27.8 |
  | diamond (4) | 9.7 | 8.42 | 29.6 |
  | convert_sample_rate (5) | 8.1 | 8.42 | 29.7 |
  | decode_c (4) | 13.2 | 9.18 | 27.2 |
  | filter_c (4) | 13.4 | 9.22 | 27.1 |
  | decode_c_single_field | 13.6 | 9.35 | 26.7 |
  | detect_motion (4) | 12.7 | 8.91 | 28.0 |
  | combine_still_and_moving | 12.0 | 8.49 | 29.4 |

  Two passes are the decoder: the horizontal sample-rate conversions (5.2 ms per field)
  and the diamond interpolation filters (3.6), and leaving out either one alone puts the
  demodulator at its 8.4 ms budget and the playback at 29.6 of 30 frames/s.  The
  per-pixel passes (rescale, dropout, de-emphasis, decode_c, filter_c) cost nothing
  measurable even four dispatches at a time, so dispatch overhead is not the decoder's
  problem.  What the two expensive passes do per output: `convert_horiz_sample_rate.comp`
  runs the 65-tap 4:3 filter (which its comment says was sized for the audio path,
  "performance doesn't matter much") over the two inter-frame luma fields, about 22
  source taps per output with a bounds check, a global float16 load and a shared-memory
  tap load each, plus three integer divisions per invocation by the upsampling factor,
  a push constant; `filter_diamond.comp` visits 24 taps of the 7x7 kernel per output, 8
  of them zero, each through a four-way edge reflection, and loads the kernel from a
  shared tile behind a barrier.  Candidates, in order: a video-length 4:3 filter (about
  25 taps, the 2:3 one has 23) and the upsampling factor as a specialization constant;
  the diamond with its 16 nonzero taps as literals and the reflection only on the edge
  rows and columns; the equalizer's identity case without the shared-memory FIR (0.6 ms
  for a float-to-half conversion); the two pow() per channel in combine (1.3 ms).
- **The 4:3 conversion** (2026-10-19, done, uncommitted): two changes, measured apart
  with the `MUSELD_VIDEO_43_FILTER=audio` hook (the video through the audio's filter).
  (1) `convert_horiz_sample_rate.comp` divided by the upsampling factor, a push constant,
  three times per invocation; it is always 3 and is now a compile-time constant
  (bit-identical output, checked on an exported frame).  (2) The inter-frame luma ran
  through the audio's 65-tap filter: until 2024-08 the two shared a 31-tap rectangular
  design, then the audio got its Kaiser (commit 2a6bf7b, "only used for the audio
  signal") and the video came along.  The video now has its own 37-tap least-squares
  filter to the same specification (`tools/muse-4to3-filter.py`; pass band to 0.125 of
  the 3x rate, stop from 0.167, 34 dB and more down), the audio keeps its 65.  On the
  detailed frame at 60 s of blue1 the two filters differ by 0.6 rms levels, within +-2
  along edges, nothing visible.  A10X, real-time s8 playback with audio:

  | | decoder ms/field | demod ms/block | frames/s | underruns/100 s |
  |---|---|---|---|---|
  | before (65 taps, divisions) | 13.3 | 9.16 | 27.2 | 41 |
  | constant factor, 65 taps | 12.2 | 8.62 | 28.9 | 11 |
  | constant factor, 37 taps | 12.1 | 8.47 | 29.6 | 3 |

  The divisions were most of it; the taps are worth 0.15 ms per block more, which at
  this margin is 8 underruns.  The demodulator is now at its 8.4 ms budget: 29.6 of 30
  frames/s.  Also fixed: the shader's shared tap tile was 64 entries for a 65-tap filter.
  Next: the diamond filter (3.6 ms per field), the same kind of work.
- **The diamond filter** (2026-10-19, done, uncommitted): `filter_diamond.comp` now has
  the kernel's 16 nonzero taps as literals (the 7x7 kernel is fixed; at a missing
  lattice point only the taps at odd Manhattan distance meet known pixels), no shared
  tile or barrier, no tap buffer, and the edge reflection only on the three-pixel
  border; the interior reads 16 pixels straight from the buffer.  The output is within
  +-2 levels of the old shader's (fp16 summation order; 16% of the pixels differ by 1
  or 2 on the 60 s frame).  A10X, real-time s8 playback with audio: decoder 8.4 ms per
  field (was 12.1), demodulator 8.39 ms per block, its budget, 29.8 of 30 frames/s,
  2 underruns in 100 s.
- **Where the limit is now** (2026-10-19): unpaced (`--no-sync`) the s8 clip still runs
  29.7 frames/s, and the same without video, so the display and the decoder are not it.
  `ResamplingFrameReader` now logs its resampling time against its wait for demodulated
  input per frame at `--log P3` ("reader avg/frame"): 9-10 ms of resampling per 33 ms
  frame and, without video, 24 ms of waiting for the demodulator -- the reader thread
  is not it either.  The demodulator sits at 8.4 ms per block whenever its input is
  the real HTTP read, against 5.25 in repeat mode: with every GPU pass skipped
  (`MUSELD_SKIP_PASS=bandpass_fir,...`) the read alone is 4.9 ms per block, 106 MB/s,
  1.7x real time; the GPU chain alone is 5.25, 1.6x; together they give 8.4, so the
  receive's CPU time and the GPU chain do not overlap on this box (the three-core
  CPU serving the GPU driver and the TCP receive at once, as noted under Input).
  So the ceiling is the pair, each 1.6-1.7x on its own, and real time is where they
  meet.  Raising it means either less CPU per received byte or a cheaper chain; the
  playback itself is at real time with 2 underruns in 100 s, which is the goal.  The
  FLAC capture from the NAS is unchanged, read-bound at 10.4 ms per block (the FLAC
  decoding on the reader's thread), 189 underruns in 100 s.
- **The decoder alone** (2026-10-19, in progress): `--write-muse16` wrapped negative raw
  levels (fixed, commit 17d7b8f), which is why the first attempt decoded in intra-field
  mode.  With a correct dump (the ldf from 60 s, 24.5 s long, served from the Mac with
  the range server) `--input-type muse-16 --no-sync --no-audio` runs the decoder and the
  file-mode reader with no demodulator; the decoder's P3 line now reports its input
  wait separately ("input wait"), so elapsed minus wait is the decoder's own time.  On
  the M2: 2.2 ms per field, 104 frames/s.  The A10X run is pending: the Apple TV was
  unreachable (devicectl "not able to fulfill the requested usage assertion", the box
  asleep with its owner away).  Recipe: `python3 rangeserver.py <dir> 8765` on the Mac,
  then launch museld on the box with `http://<mac>:8765/<dump>`.
- **The feed-forward MUSE reader** (2026-10-20, merged from master, commit 818f4e6 there):
  `MuseFrameReader` with `timebase/LineTimebase` replaces `ResamplingFrameReader`'s
  per-sample DPLL.  A10X, the s8 clip from the NAS: the reader's timebase plus
  resampling is 3.3-3.7 ms per frame against 9.6-10 for the DPLL (the M2: 1.1 against
  3.2); real-time playback with audio 29.5 frames/s, 1 underrun in 100 s, the
  demodulator at 8.36 ms per block; the decoder's noise statistics unchanged on the
  Mac (SNR 34.1 dB, 43.4 after de-emphasis).  Unpaced it is still 29.7 frames/s: the
  ceiling is the receive-plus-GPU pair as measured the day before, and the reader's
  6 ms of CPU per frame given back did not move it.  The merge kept this branch's
  carrier rotation in `fm_quadrature.comp` over master's phase unwrap: both pick the
  reading nearest the carrier, the rotation by construction, so master's `unwrap_below`
  push constant went and its explanation (the Technidisc title-text wrap) moved into
  the shader's comment.
- **s16 and lds input** (2026-10-20, with the feed-forward reader): neither plays in real
  time on the A10X from the NAS; s8 remains the raw format for this box.  The clips are
  the same 60 s of blue1 as s8 (62.5 MB/s), lds (78 MB/s; made with ldconv from the s8,
  now beside it on the NAS) and s16 (125 MB/s).  Real-time playback with audio: s16
  read 16.4 ms per block, 22.2 total, 38% of real time, 272 underruns; lds read 9.1 ms,
  14.9 total, 17 frames/s, 215 underruns; s8 at real time.  The receive alone (every
  GPU pass skipped) from the NAS reached 123 MB/s on lds at one moment and 46-72 MB/s on
  both s8 and lds half an hour later, in alternating runs: the NAS's rate to the box
  swings by a factor of two with time, so the lds unpack's own CPU cost could not be
  separated from it today.  What is firm: with the whole pipeline running, the receive
  gets 51-72 MB/s on this three-core CPU, which carries s8's 62.5 and not lds's 78 or
  s16's 125.  (A Python range server on the Mac serves 1 GB/s over loopback but the box
  reads 50 MB/s from it across the Mac's Wi-Fi, so it is no better than the NAS for
  these tests; the Mac's LAN address also changes between days.)
- **The input server** (2026-10-22): the NAS (a Synology DS1819+) serves its share to
  multivac (FreeBSD, 8 GB RAM, Apache in a jail at 10.0.2.22) over NFS, and multivac
  serves it as `/ld-captures`.  The variability was multivac's page cache: a warm
  file goes to the box at line rate (131 MB/s), a cold one at whatever Apache's
  4 MiB-range reads get from the source: 44-85 MB/s over NFS (the NFS link itself does
  111 with 4 MiB reads, the Synology's RAID6 386 locally), 65-74 from multivac's
  spinning scratch disk (98 with one long read), 94 from its SSD (206 on loopback).
  The three blue1 clips total 9.2 GB against about 5.5 GB of cache, so alternating
  runs read cold.  Now: `/ld-local` serves `/scratch0/ld-captures` (the HDD; the three
  clips copied there) and `/ld-ssd` the SSD (the s8 clip so far), both LAN-only
  (`Require ip 10.0.2.0/24`, the port 80 vhost; nullfs into the jail, fstab).
  EnableSendfile made the 4 MiB-range pattern slower (65 against 84 MB/s on loopback),
  so the default mmap stays.  The remaining per-chunk cost is on the box's side: a cold
  SSD file is 94 MB/s to the box in 4 MiB range requests one at a time, about 12 ms of
  request overhead per chunk; bigger chunks or a second connection in
  `HttpByteSource` would recover most of the way to line rate.
- **HTTP chunks and connections** (2026-11-05, done): `HttpByteSource` can run several
  connections, claiming chunks in file order and queueing them by offset whichever
  finishes first (so a chunk's cold start-up on the server overlaps the previous one's
  transfer), with the chunk size and the connection count as `MUSELD_HTTP_CHUNK_MIB` and
  `MUSELD_HTTP_CONNECTIONS`; a unit test runs it against an in-process httplib server.
  Measured on the box, receive only, the file evicted from multivac's cache before every
  run (MB/s from block size over `total`):

  | cold source | 4 MiB, 1 conn | 4 MiB, 2 conn | 16 MiB, 1 conn | 16 MiB, 2 conn |
  |---|---|---|---|---|
  | SSD (`/ld-ssd`), s8 | 79 | 92-117 | 99 | 119 |
  | HDD (`/ld-local`), lds | 42 | 15-24 | 72-88 | 15-20 |

  Two connections on a spinning disk make its head seek between two streams 4 MiB
  apart, so the default is 16 MiB chunks on one connection, two ahead (32 MiB in
  flight); two connections remain a hook for SSD-backed servers.  Real-time lds playback
  with audio, cold: from the SSD 29.5 frames/s with 2 underruns, both in the first
  two seconds; from the HDD 28.0 frames/s with 14, in three bursts, the HDD's 72-88 MB/s
  being marginal for lds's 78.  So lds works on this box from an SSD-backed server, and
  s8 stays the safe raw format from anything; s16 remains out by the wire.
- **FLAC on the A10X** (2026-11-05): it is the decoding, not the network.  `HttpByteSource`
  now logs at P3, every 256 MiB, what it delivered and how much of the time the reader
  waited for it ("http input: ... waited for the network N% of the time"); on the blue1
  ldf from the NAS, receive only, the reader waited 0% and the prefetch thread delivered
  a block every 9.1-9.4 ms (8.39 is real time): libFLAC on one A10X core decodes this
  10-bit capture (about 6.2 bits per sample compressed, 48 MB/s of file at real time)
  at 0.88x.  The cost follows the bits in the bitstream more than the predictor: the s8
  clip re-encoded as FLAC (8-bit content in a 16-bit stream, 4 bits per sample) decodes
  at 0.97x with the usual order 12, 0.99x at order 8 and 1.06x at order 4 (the file 1.5%
  and 10% bigger); on the M2 order 4 decodes 25% faster than order 12.  So `ldconv
  --bits 8`, which the s8 clip's identical decoder statistics say costs nothing on this
  capture, brings FLAC to the edge of real time on this box, and only decoding on two
  threads (FLAC frames are independent, Ogg pages mark where they start) would put it
  clearly inside.  Not done; s8 remains the format for the 2017 box, FLAC for the Mac.
- **FLAC decoded on two threads** (2026-11-05, done): `input/ParallelOggFlacReader`.
  A scanner thread cuts the Ogg stream into runs of pages (2 MiB and more) that begin
  at a page whose first packet is a fresh frame, renumbering the pages so that each
  run follows the stream's header pages in sequence; decoder threads, each with its
  own libFLAC decoder, take the runs in turn (headers, then the run, then end of
  stream) and the frames' own sample numbers put the output back in order.  Seeks
  bisect the pages by granule position, which also frees seeking from the wrapped
  36-bit STREAMINFO count.  The factory uses it for seekable FLAC-in-Ogg on a machine
  with three cores or more, `MUSELD_FLAC_THREADS` overrides (1 is the serial reader);
  plain FLAC and live streams keep the serial reader.  Tests against libFLAC's own
  encoder: order on 1-3 threads, identical to the serial reader, seeks of every kind
  including while runs are in flight; on the Mac the exported frame is identical.
  Throughput as blocks per 8.39 ms, receive only: the M2 from 4.13 ms per block
  serial to 2.24 on two threads and 1.80 on four; the A10X on the 10-bit blue1 ldf
  from the NAS from 9.1 ms (0.92x) to a reader that waits for the network 61% of the
  time at 55 MB/s, i.e. the NAS path is now the limit there, and on the 8-bit-content
  clip from multivac's SSD 5.5-5.9 ms (1.5x).  Real-time playback with audio on the
  A10X: the ldf from the NAS at 8.4 ms per block with 6 underruns in 100 s, clustered
  where the GPU wait rose to 7.5 ms (the two decoder threads and the receive leave the
  GPU driver short of CPU); the SSD clip at 8.26 ms with none.  So FLAC plays in
  real time on the 2017 box, marginally from the NAS and cleanly from a fast source.
- **Thread roles** (2026-11-06, done): `util/ThreadRole.h` names each thread and, on Apple
  platforms, sets its QoS class: the demodulator thread user-interactive (it feeds the
  GPU), the frame reader user-initiated, and the input threads -- FLAC decoders, Ogg
  scanner, HTTP fetcher, prefetcher -- utility, since they run seconds ahead of the
  picture.  `MUSELD_THREAD_QOS=0` for A/B.  A10X, the 10-bit ldf from the NAS in real
  time with audio, six runs alternating: the chain's GPU wait 6.8 ms per block without
  the classes and 5.7-6.0 with them, the demodulator's total steady at 8.36-8.42; with
  the file warm in multivac's cache, 7, 2 and 0 underruns in 100 s without, 0 and 0
  with.  One QoS run had 17: its network statistics showed 43 MB/s delivered with the
  reader waiting 61-71% of the time, the cold NFS path below the ldf's 48 MB/s; the
  runs after it found the region warm.  So the box now plays 10-bit FLAC in real time
  whenever the server delivers 48 MB/s, and the cold NAS path is what remains: warm the
  file, keep the clips on multivac's SSD, or fix the cold path on multivac (the
  `EnableMMAP Off` trial, or nginx's read_ahead).
- **A faster libFLAC** (2026-11-07, done): a sampling profile of the decode on ARM64
  (the M2 runs the same code paths as the A10X: libFLAC 1.5.0 has no NEON decoder
  routines) splits the time 44% LPC restore (the prediction, plain unrolled C), 38%
  Rice residual decoding (the bit reader, inherently serial), 13% the FLAC frame
  CRC-16 and the Ogg page checksum, 5% the rest; the 10-bit samples keep the decoder
  on the 32-bit restore path thanks to their wasted bits.  `tools/lpc-neon-bench.cpp`:
  a NEON restore sample by sample gains 5% (the loop is a serial recurrence); four
  outputs per step, the known part of the four sums built in parallel and a six-term
  serial tail, gains 2.85x on the M2, bit-identical.  That is now a patch to the
  libFLAC the tvOS build fetches (`cmake/patches/flac-1.5.0-lpc-restore-neon.patch`,
  applied once after extraction by `cmake/patch-flac.cmake` as FetchContent's
  PATCH_COMMAND; the macOS and Linux builds use their system libFLAC, unpatched):
  `FLAC__lpc_restore_signal` takes the NEON path for orders up to 12 on ARM64 with
  NEON, the first 12-order outputs scalar so nothing reads before the warm-up.
  Verified natively: the patched library decodes 3 GB of the blue1 capture
  byte-identically to the stock one, in 33% less time like for like (14.8 to 9.9 s).
  A10X, the 10-bit ldf, receive only: one decoder thread from 9.6 to 6.3 ms per block
  (1.33x real time), two from 6.4 to 5.0 (1.68x).  Real-time playback with audio then
  turned the thread question around: with the faster decoder the serial reader on one
  thread runs clean (two runs, 0 underruns, the demodulator at 8.39 ms per block with
  a maximum of 8.42, GPU wait 5.75), while the parallel reader costs the GPU driver
  its CPU on the three cores: 6-11 underruns with two decoders at either QoS class,
  37 with one decoder through its scanner and queues.  So on tvOS the default is now
  one thread, the serial `LdfInputReader`; the parallel reader stays the default on
  machines with three cores or more (the M2: 3.7x real time on two threads) and
  `MUSELD_FLAC_THREADS` overrides.  Two wasted measurements on the way, both caught by
  the binary's date: Xcode rebuilt the patched library without relinking the app.
  10-bit MUSE FLAC from the NAS now plays in real time on the 2017 box with margin.
- **The menu and the browser** (2026-11-08/09): the on-screen menu (`ui/Menu`, `PlayerMenu`,
  `MenuOverlay`; the keys' actions shared through `PlayerActions`) and the file browser
  (`ui/Browser` with `DirectoryListing`, `TextEntry`, `SavedState`), documented in
  docs/museld.md.  The Apple TV app starts in the browser; the Menu button opens the
  menu during playback and from its root page leaves the app.  The rendering goes
  into the decoded frame like the OSD, so on the box it costs the text renderer's
  dispatches per line while open and nothing while closed.  A recent file chosen from
  the Open page came through as an empty name ("File not found: "): the item's lambda
  rebuilt the Open page, which destroyed the lambda and the captured address it had
  passed by reference.  `Browser::go` and `showListing` copy their argument first, and
  `Menu` calls a copy of an item's function (2026-11-10).  An input the browser chose
  that cannot be played (the `exit(EXIT_FAILURE)` paths of `play_input`, now `InputError`,
  and a reader's exception) returns to the browser with the message in a bar along the
  bottom, until a key is pressed or 15 s pass, instead of ending the app (2026-11-10).
  A review of the merged branch (2026-11-11) fixed: the address field's DONE closed the
  menu from inside the entry's own callback, destroying the running `std::function`
  (`Menu::close` now defers the entry's end to `dispatchToEntry`); a PC keyboard's space
  bar reached the field twice, as text and as the on-screen select (the text event's space
  is dropped); `fill_rect.comp` read and wrote one rgba32f image, the construct the A10X's
  Metal refused for the subtitle shader (the frame is bound twice now); a web server's
  index page was fetched on the main thread, up to 15 s of frozen picture (`Browser::poll`
  applies a background fetch); the arrows of the menu were bytes 0x7f/0x80, and a UTF-8
  continuation byte drew as the down arrow (0xfe/0xff now); `seekOrPan` dropped a refused
  seek's result, moving the subtitle clock while the picture stayed; the parallel FLAC
  reader's decoders had no bound on the decoded runs waiting for the reader, and a seek
  into a frame that spans Ogg pages started its run on the continuation page, a frame late.
- **Merged to master** (2026-11-10) after the picture regression suite was run against
  master's goldens on the Mac.  It found one bug: the demodulator's read-ahead made the
  player's first iterations return on the input timeout, and `--write` wrote the images
  as they were (zeros, so green frames) once per such iteration, shifting every frame and
  the audio; the write now takes decoded fields only.  What remained: NTSC and PAL within
  tolerance at 50-105 dB PSNR (the quadrature shader's rotation before the atan, rounding
  level), MUSE at 48-55 dB from the 35-tap band-pass and the 37-tap 4:3 video filter in
  about equal parts (reverting both: 65-72 dB; the rest the diamond filter's border), all
  of it edge ringing at the top of the horizontal band, 0.4% of white rms on the worst
  frame.  The goldens were re-recorded on the Mac (M2, the Apple GPU) from this branch,
  so a run there passes bit-exact and another machine shows GPU-level DIFFs, as before
  in reverse.
- **First run of the merged build on the box** (2026-11-10): three things.  (1) The app
  died in Vulkan set-up: `fill_rect.comp` (the menu's panels) loaded and stored the same
  rgba32f image, which Metal compiles as a read-write texture, and the A10X (read-write
  texture tier 1) has that for 32-bit single-channel formats only -- the shader compiler
  service failed (`XPC_ERROR_CONNECTION_INTERRUPTED`) and MoltenVK reported
  `ErrorInitializationFailed`.  The image is now bound twice, read-only and write-only, as
  `render_subtitle.comp` always did.  Rule: a shader that reads and writes one image binds
  it twice.  (2) A devicectl console launch sends the app a SIGTERM as it comes up (seen with
  `MUSELD_TRACE_EVENTS=1`, which prints SDL's events and takes the signals itself), and SDL
  turned it into its quit event, so the browser screen left at once; from the home screen
  the app was fine.  tvOS has no terminal, so `SDL_HINT_NO_SIGNAL_HANDLERS` is set there.
  (3) The address field started SDL text input, which on tvOS is the system's full-screen
  keyboard dialog, over the menu's own keyboard; not started on tvOS now.
  Then from using it (2026-11-11): a web address without its slash went to the player
  (HTTP status 301); the browser now takes a web address for a directory unless its
  name has a capture's extension, follows the server's redirects and keeps the field's
  last text for correcting.  Opening a file from the browser took a second or two
  with the Open page still up and its cursor reset: a bar says "Opening ..." over a
  cleared screen meanwhile (and the player's OSD says OPENING when its Open page
  chose), and the keys pressed while waiting are dropped before the player's first
  poll -- a swipe on the remote then had started a chapter search ("NO CHAPTER CODE").
  A TV remote over HDMI-CEC (no Siri Remote at hand) sends the arrows, Return for OK and
  Escape for back, nothing else (logged at M4 on 2026-11-11), so the player is driven with
  those: Back opens the menu over the picture and, from its root, closes it again rather
  than leaving; the menu's Stop item returns to the browser, whose Back leaves the app.
  The aspect mode set for a disc was gone when it was opened again: the state file now
  keeps, per address, the aspect, overscan, audio track and subtitle choice (the disc's
  own properties; not full screen, which is a testing convenience on a PC, nor the decode
  toggles), applied over the command line's defaults when the disc is opened.
  Input: cpp-httplib received in 16 KiB pieces, which read 45 MB/s on the A10X from a
  gigabit LAN; 256 KiB pieces (`input/HttpClient.h`) read 60-100 MB/s, the NAS varying
  run to run.  Receiving at 100 MB/s (the raw s16 at 125 MB/s wanted) costs the
  three-core CPU enough that the GPU is starved of its driver's time and makes little
  progress during the read, so s16 raw is out on this box; s8 at 62.5 MB/s is fine,
  and a FLAC capture (about 66 MB/s of file) decodes on the reader's thread without
  trouble -- the earlier reading that FLAC decoding was a limit was the 16 KiB pieces.
  Start-up first died in the memory allocator
  (fixed: a request above its 16 MB block size, which MoltenVK's requirements for the
  MUSE frame buffers are on the A10X).  Audio underruns throughout, as expected below
  real time.  This is the box's limit until the MUSE chain itself gets cheaper; a
  2021+ Apple TV (A12/A15) is the realistic MUSE target, as the plan said.
- **Captures from the NAS**: `http://10.0.2.22/ld-captures/...` on the LAN, plain http
  and no password (the web jail's vhosts.conf, see multivac's web.md 2026-10-18).  The
  40 MHz Fight Club capture from there: real time, the http read under 1 ms per block.
- The Apple TV sleeps after a while and then refuses to launch apps ("System is
  asleep - foreground app launch forbidden"): wake it with the remote first, or set
  Settings > General > Sleep After to Never while testing.
- The Siri Remote as a gamepad (needs the original remote), and Menu/back quitting the
  app: fine for development, but a tvOS app is not supposed to exit itself; a
  settings screen that stays in the app replaces this later.
- A `--seek` and `--pause` from the Xcode scheme work; a settings screen is for later.
- The app icon and the top-shelf image (2026-11-12): a laserdisc and the name, drawn by
  `player/tvos/make-icons.py` (numpy + PIL, the bundled Noto Sans) into
  `player/tvos/Assets.xcassets`, which is checked in -- the build does not run the
  script.  The icon is tvOS's stack of three layers (background, disc, wordmark) that
  move against each other under the focus; `--preview <png>` renders the flattened icon
  and the top shelf to look at.  CMake adds the catalog to the bundle and names it with
  `ASSETCATALOG_COMPILER_APPICON_NAME`; Xcode's actool writes `Assets.car` and the
  `CFBundleIcons`/`TVTopShelfImage` keys into the Info.plist.

### 2. Measure the rest

`--log P3` per-stage timing.  Still to do: the UDP receive thread alone with
`ethadc-tx` at 62.5 MHz (42,000 datagrams/s is CPU work on an A10X); live NTSC over
UDP; MUSE.  Raise nothing to real time until the demodulator keeps up.  On the
receiving side the socket buffer is the first thing to lose packets; the `udp://`
input logs what it got.

## Gotchas already met

- `third_party/<lib>/` must not be an include directory: a file named `VERSION` in it
  stood in for `<version>` on macOS/Windows (case-insensitive file systems).
- `ethadc-tx` keeps sending through ICMP port-unreachable; the FPGA would.  museld
  binds the port three times at start-up (header probe, type probe, reader) and loses a
  couple of seconds of the stream meanwhile, which is harmless for a live source.
- The pixel aspect is 1.0 under SDL (no EDID size available); it measured 1.000 on
  every display tried under GLFW.
- After a CMakeLists change, the first `cmake --build` with the Xcode generator
  regenerates the project but finishes compiling with the old one; build twice, or
  run `cmake --preset …` first.
- `cmake --build --preset` must run from `player/` (where `CMakePresets.json` is).
