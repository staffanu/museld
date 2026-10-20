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
  An .srt file cannot be handed to the app yet: `--subtitles-file` opens local files
  only, and a file put into the app's data container with `devicectl device copy to`
  is there but the app's stat of it fails with "Operation not permitted".  A leading
  `~/` in file arguments is expanded (`expandHome`) for when that works.
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
- A `--seek` and `--pause` from the Xcode scheme work; a settings screen, an app icon
  and a top-shelf image are for later.

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
