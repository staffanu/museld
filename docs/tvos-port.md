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

## To do, in order

### 1. Cross-compile the core

A CMake preset for tvOS: `CMAKE_SYSTEM_NAME=tvOS`, `CMAKE_OSX_ARCHITECTURES=arm64`,
`CMAKE_OSX_SYSROOT=appletvos`, the Xcode generator (signing and deployment need it),
`MUSELD_ARCH` empty, `USE_LIBAV=OFF`, `USE_OCR=OFF`, `BUILD_AC3RF_DECODE=OFF`.  Expect to
touch: the `-mmacosx-version-min=15.0` flag (macOS only), `pkg_check_modules(FLAC)` (no
pkg-config for tvOS: build libFLAC from source with FetchContent, as the SDL3 fallback in
`src/CMakeLists.txt` already does for SDL; both projects support tvOS), and anything
using APIs tvOS lacks (`system()`, `fork`, `getenv` of the shell kind).  The AVX code is
behind x86 guards and the NEON path exists.  miniaudio's Core Audio backend covers tvOS.
cpp-httplib and the ethadc receiver are plain BSD sockets.

### 2. Vulkan through MoltenVK, no loader

tvOS cannot load a Vulkan loader from outside the bundle: link MoltenVK statically
(`MoltenVK.xcframework` from the LunarG Vulkan SDK has tvOS slices; or build it from
source).  `vulkan.hpp`'s dynamic dispatcher is given MoltenVK's `vkGetInstanceProcAddr`
directly.  The surface comes from `SDL_Metal_CreateView` / `SDL_Metal_GetLayer` and
`vkCreateMetalSurfaceEXT` (extension `VK_EXT_metal_surface`), not `SDL_Vulkan_CreateSurface`,
which wants to dlopen a Vulkan library -- do this on macOS as well, under `__APPLE__`, which
also removes the libvulkan dlopen the macOS packages rely on (docs/packaging.md).
`VK_KHR_portability_enumeration` / `portability_subset` apply as on macOS (VulkanManager.h).
The SPIR-V shaders (`src/shaders/*.spv`) and the subtitle font go into the bundle as
resources (`MACOSX_PACKAGE_LOCATION Resources`); `get_executable_dir()` in museld.cpp
resolves to the bundle, so the lookup needs only the Resources subdirectory added.

### 3. App shell

- `#include <SDL3/SDL_main.h>` in museld.cpp: on Apple platforms `main` becomes the UIKit
  entry point; elsewhere it is unchanged.  SDL3 pumps the UIKit run loop from
  `SDL_PollEvent`, so the blocking main loop (`runPlayer`) can stay.
- `SDL_EVENT_WILL_ENTER_BACKGROUND` / `DID_ENTER_FOREGROUND`: pause decoding and stop
  submitting GPU work in the background, or tvOS kills the app.  Handle in
  `InputController::poll`, which already owns the event queue.
- Siri Remote: SDL presents it as a gamepad (`SDL_INIT_GAMEPAD`, `SDL_EVENT_GAMEPAD_BUTTON_DOWN`,
  touch surface as axes/d-pad).  Map onto the existing actions: play/pause -> Space,
  d-pad left/right -> seek, select -> pause, Menu -> quit (SDL lets Menu exit the app
  unless `SDL_HINT_TV_REMOTE_AS_JOYSTICK` / the back-button hints say otherwise).
- No command line on tvOS, but Xcode scheme *launch arguments* arrive as `argv`, so for
  testing the app is launched with the same options as on the desktop, e.g.
  `--log P3 udp://:5000` or `http://nas/video/capture.ldf`.  A settings screen is later.
- Info.plist: `NSLocalNetworkUsageDescription` (and `NSBonjourServices` if ever used),
  `UIRequiresFullScreen`, no `NSAppTransportSecurity` needed for raw sockets.

### 4. Measure on the A1842

`--log P3` per-stage timing.  Order: the UDP receive thread alone with `ethadc-tx`
at 62.5 MHz (42,000 datagrams/s is CPU work on an A10X); NTSC from a file over HTTP;
live NTSC over UDP; MUSE.  Raise nothing to real time until the demodulator keeps up.
On the receiving side the socket buffer is the first thing to lose packets; the
`udp://` input logs what it got.

## Gotchas already met

- `third_party/<lib>/` must not be an include directory: a file named `VERSION` in it
  stood in for `<version>` on macOS/Windows (case-insensitive file systems).
- `ethadc-tx` keeps sending through ICMP port-unreachable; the FPGA would.  museld
  binds the port three times at start-up (header probe, type probe, reader) and loses a
  couple of seconds of the stream meanwhile, which is harmless for a live source.
- The pixel aspect is 1.0 under SDL (no EDID size available); it measured 1.000 on
  every display tried under GLFW.
