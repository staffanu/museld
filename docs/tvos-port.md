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
