# Packaging and Releases

This document describes the binary packages published for Windows and macOS: what is in
them, how CI produces them, and how to cut a release. For building from source — the only
option on Linux — see the [README](../README.md).

## What is published

Six packages, three per platform, all built from the same commit:

| Package | Zipped | Contents |
|---|---|---|
| `museld-windows-x86_64.zip` | ~7 MB | `museld.exe`, `ac3rf-efm-decode.exe` |
| `museld-windows-x86_64-full.zip` | ~70 MB | the same, plus FFmpeg for `--write` and ONNX Runtime for `--ocr` |
| `ac3rf-efm-decode-windows-x86_64.zip` | ~2 MB | `ac3rf-efm-decode.exe` alone |
| `museld-macos-universal.zip` | ~10 MB | `museld`, `ac3rf-efm-decode` |
| `museld-macos-universal-full.zip` | ~55 MB | the same, plus FFmpeg for `--write` and ONNX Runtime for `--ocr` |
| `ac3rf-efm-decode-macos-universal.zip` | ~1 MB | `ac3rf-efm-decode` alone |

(The full-package sizes are estimates until a CI run confirms them.)

Each is self-contained: it carries the libraries the programs need, so nothing has to be
installed first. Only the graphics driver comes from the system.

The minimal/full split exists because FFmpeg and ONNX Runtime dominate the size — FFmpeg
alone is 92 of the 100 DLLs the Windows museld would otherwise need, all to encode H.264
and AAC. The minimal build turns both off (`-DUSE_LIBAV=OFF -DUSE_OCR=OFF`);
`ac3rf-efm-decode` links neither of them nor Vulkan/GLFW, so it also gets a package of
its own, which is what most people who only want the audio decoded need. Using `--write`
in a minimal build fails with "FFMPEG is not available", and `--ocr` with "requires a
build with -DUSE_OCR=ON".

The macOS packages are universal and run natively on both Apple Silicon and Intel. They
need macOS 15 or newer, which is what the Homebrew libraries they are built against
require.

## Where they come from

Everything is built by [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) on every
push, and can also be started by hand from the Actions tab (`workflow_dispatch`). The jobs
that package:

| Job | Runner | Produces |
|---|---|---|
| `windows-package` | `windows-latest`, MSYS2 UCRT64 | the three Windows zips |
| `macos-build` (matrix) | `macos-latest` (arm64), `macos-15-intel` (x86_64) | one staged tree per architecture |
| `macos-package` | `macos-latest` | the three macOS zips |

Homebrew only ever has libraries for the architecture it is installed on, which rules out
asking clang for both at once. So each architecture is built where it is native, and
`macos-package` joins the two trees with `lipo`; the trees travel between the jobs as
tarballs, because artifacts do not preserve the executable bit.

The work lives in scripts under [`packaging/`](../packaging) rather than in the workflow,
since each runs three times:

```
packaging/windows/stage-package.sh     collect binaries, assets and DLLs into a package
packaging/windows/smoke-test.sh        check a package
packaging/macos/stage-package.sh       the same for one macOS architecture
packaging/macos/merge-universal.sh     lipo two staged trees into a universal one
packaging/macos/smoke-test.sh          check a package
packaging/macos/museld-launcher.sh     shipped as "museld"; finds the Vulkan driver
packaging/*/README-*.txt               the README that goes inside each package
```

## Making a release

Push an annotated tag beginning with `v`; the annotation's subject and body become the
text on the release page:

```bash
git tag -a v0.1.0    # write the release notes as the tag message
git push origin v0.1.0
```

Every package job attaches its zips to the GitHub release for that tag. Nothing is
published without a tag: on an ordinary push the same zips are only uploaded as CI
artifacts, which are kept for 90 days and need a GitHub login to download — useful for
handing someone a build of master, not for release.

Check afterwards that all six assets are on the release page, since a failed platform does
not stop the others from publishing.

## Things that are easy to get wrong

**The CPU baseline.** `-march=native` targets the CPU doing the build, so a package built
that way on a CI runner can die with an illegal instruction on a user's older machine. The
`MUSELD_ARCH` cache variable exists for this: it defaults to `native`, which is right for
local builds, and the packages set it to `x86-64-v2` (SSE4.2, Nehalem and later). On arm64
macOS it is set to nothing at all, leaving clang's default: every Mac that can run these is
an M1 or later.

The baseline decides only which CPUs can run the binary, not whether AVX is used:
`firFilterAvx` in `filter/FirFilterStage.cpp` carries a function-level `target("avx")`
attribute, so it is compiled on every x86 build whatever `-march` says, and
`FirFilterStage::simdSupported()` picks it via CPUID at runtime. Raising the baseline
therefore buys very little and locks out old CPUs; it used to be set to `x86-64-v3`
precisely because the AVX paths were gated on `__AVX__` and lowering it would have made
them silently disappear. Whoever chooses `use_simd` must ask `simdSupported()` rather than
assume — passing `true` unconditionally is a crash on any CPU below the AVX line.

**Files museld loads at runtime.** museld finds its SPIR-V shaders and subtitle font
relative to its own executable, so `shaders/` and `fonts/` have to travel with it; a bare
binary starts and then fails. This is also why the macOS launcher runs `museld-bin` from
the package root rather than a `bin/` subdirectory.

**Vulkan on macOS.** The loader discovers drivers through manifests in a handful of fixed
system directories, none of which a downloaded folder is in. The package therefore carries
MoltenVK and its manifest, with `library_path` rewritten to be relative to the manifest so
that it resolves wherever the package is unpacked, and the `museld` launcher points the
loader at it through `VK_DRIVER_FILES` before running the real binary. Users must run
`./museld`, not `./museld-bin`. On Windows nothing of the sort is needed: the loader is
bundled next to the binaries and finds the driver through the registry.

**Signatures.** `lipo` discards them, so anything it touches is signed again afterwards.
`install_name_tool` invalidates them too, hence the signing at the end of the macOS
staging. Signing is ad-hoc (`codesign --sign -`), which is what the arm64 kernel requires
to run a binary at all, but is not a paid Developer ID and is not notarised, so Gatekeeper
still objects — see below.

**Adding a dependency** needs nothing here: both staging scripts resolve the closure
themselves, with `ldd` on Windows and `otool -L` on macOS, and copy whatever is new.

## How the packages are checked

CI runs each package before it can be uploaded, which is the part that catches a forgotten
library:

* On Windows, the binaries run with MSYS2 stripped off their `PATH`, so nothing can be
  picked up from `/ucrt64/bin` by accident.
* On macOS there is no PATH to strip — a Mach-O file names the exact path of everything it
  loads — so the check reads those names back with `otool -L` and fails if any still
  points into Homebrew. Merged packages are also checked with `lipo -archs` for both
  slices.
* Both then run `ac3rf-efm-decode --version` and `museld --help`, which returns before any
  Vulkan device is needed and so works on a runner without a GPU.

What this does not prove is that museld actually renders anything: no CI runner has a
usable GPU — that has to be checked by hand. The Windows build has been exercised that way
against real captures; the macOS museld build remains experimental for want of it.

## Code signing

Neither platform's download is signed with a paid certificate, so both object the first
time. This is documented in the README inside each package.

On macOS, the binaries are ad-hoc signed. Gatekeeper does not trust that, so a package
downloaded with a browser — which tags it as quarantined — is refused with "cannot be
opened because the developer cannot be verified". Clearing the tag once is enough:

```bash
xattr -cr museld-macos-universal
```

Downloading with `curl` avoids the problem entirely, since quarantine is applied by the
program doing the downloading. The old right-click-and-Open trick no longer works; macOS
Sequoia removed it, leaving System Settings > Privacy & Security > "Open Anyway".

Signing this properly means an Apple Developer Program membership and notarising each
build — `codesign --options runtime`, `notarytool submit`, `stapler staple` — which is
automatable in CI with the certificate and an API key as secrets, but costs money yearly.

On Windows the equivalent is milder: SmartScreen warns, and "More info" then "Run anyway"
proceeds. An Authenticode certificate would remove it.

## Author and License

Software written by Staffan Ulfberg 2021–2026.

Licensed under the [GNU General Public License](../gpl-3.0.txt), version 3 or (at your option) any later version.
