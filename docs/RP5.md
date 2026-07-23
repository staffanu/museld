# Raspberry Pi 5

This document records an investigation into whether museld can play back MUSE or NTSC on a
Raspberry Pi 5. The short answer is no, and not by a margin that tuning can close: NTSC
decodes at about **1 frame per second against a 29.97 fps target**, and the Pi's V3D GPU
turns out to be no faster than software rendering on the four Cortex-A76 cores.

The investigation is still written up here because the reasons are specific and several of
them are portability bugs that matter beyond this board.

Measured on a Raspberry Pi 5 Model B (4 GB), Debian bookworm, kernel 6.1.69, Mesa 24.2.8
(V3DV, `V3D 7.1.7.0`), on 2026-07-23. Captures: a 40 MHz NTSC `.lds` and a `muse-16`/`u16`
MUSE capture.

## Verdict

| | V3D GPU | llvmpipe (4× A76) | target |
|---|---|---|---|
| NTSC, dropout concealment on | 0.92 fps | 1.04 fps | 29.97 fps |
| NTSC, `--no-dropout` | 1.14 fps | 1.31 fps | 29.97 fps |

MUSE is likewise a few frames per second.

That llvmpipe — a software rasteriser — beats the actual GPU is the result that settles the
question. V3D is not acting as an accelerator here, so "port the pipeline to the GPU
properly" is not an available fix. Even eliminating every inefficiency identified below
would leave playback several times short of realtime.

RF demodulation alone, GPU time only, is 4.2× over budget for NTSC at 40 MHz and 6.0× for
MUSE at 62.5 MHz — before any decoding happens.

## Building on the Pi

Two obstacles, neither related to performance.

Debian bookworm ships GCC 12, which has no `<format>`, and museld uses `std::format`
throughout. GCC 13 or newer is in neither bookworm nor bookworm-backports. Use clang with
libc++:

```bash
sudo apt-get install clang-19 libc++-19-dev libc++abi-19-dev libflac++-dev
```

Bookworm's Vulkan-Hpp is 1.3.239, but the code uses `vk::detail::DispatchLoaderDynamic`,
which needs 1.3.29x or newer. The headers are header-only, so a newer copy is enough; the
1.3.239 loader is fine to keep.

```bash
git clone --depth 1 --branch v1.4.309 https://github.com/KhronosGroup/Vulkan-Headers.git
cmake -S player -B player/build-pi5 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=clang++-19 \
  -DCMAKE_CXX_FLAGS="-stdlib=libc++ -isystem $PWD/Vulkan-Headers/include" \
  -DCMAKE_EXE_LINKER_FLAGS="-stdlib=libc++" \
  -DMUSELD_ARCH=native -DBUILD_PYTHON=OFF -DUSE_LIBAV=OFF -DBUILD_AC3RF_DECODE=OFF
cmake --build player/build-pi5 --target museld -j4
```

libFLAC++ is built against libstdc++ but links against a libc++ museld without trouble —
its API does not pass standard library types across the boundary.

## What V3D cannot do

`vulkaninfo` on V3D 7.1 reports, against what the code assumes:

| Feature or limit | V3D | museld assumes |
|---|---|---|
| `shaderFloat16`, `shaderInt16`, `shaderInt8` | not supported | required by `checkDeviceFeaturesSupport` |
| `maxComputeWorkGroupInvocations` | 256 | `c_default_linear_workgroup_size` is 1024 |
| `maxComputeSharedMemorySize` | 16384 | one shader uses 16896 |
| `maxPerStageDescriptorStorageBuffers` | 8 | one pipeline layout uses 10 |
| memory types | one, `DeviceLocal\|HostVisible\|HostCoherent` | `eHostCached` on every host-read path |

Without the `shaderFloat16` exception, `checkDeviceFeaturesSupport` rejects V3D outright and
museld silently falls back to llvmpipe — which is what happens on an unpatched build, and is
worth knowing when reading any Pi benchmark.

### The 16-bit shaders run anyway

Every `muse/` and `ntsc/` decode shader does real `float16_t` arithmetic, not merely 16-bit
storage, and V3D has no 16-bit ALU. They nonetheless produce correct pictures, because
nothing at runtime enforces the rule: SPIR-V capability bits are checked only by the
validation layer, never by V3DV. Mesa's NIR compiler sees 16-bit operations on a device
without them and promotes the lot to 32-bit.

With `VK_LAYER_KHRONOS_validation` enabled the violation is explicit:

```
vkCreateShaderModule(): The SPIR-V Capability (Float16) was declared,
but none of the requirements were met to use it.   [VUID-VkShaderModuleCreateInfo-pCode-01091]
```

Exporting the same frame from V3D and from llvmpipe, which does support fp16, confirms the
lowering is correct rather than merely plausible: 94.2% of subpixels are bit-identical, the
maximum difference is 3/255 and the mean 0.058/255. That residual is the signature of the
two devices computing at *different* precision — fp32 on V3D against genuine fp16 on
llvmpipe — which is what promotion predicts.

So **no fp32 shader port is needed for the Pi**. The same applies to the `Int8` capability.
The code is formally out of spec on V3D, along with the shared-memory and storage-buffer
overruns in the table above; all four are silent today and would be worth fixing on
portability grounds, since another constrained GPU need not be as forgiving.

## Where the time goes

Per NTSC field, against a 16.7 ms budget, on V3D:

| Shader | Time |
|---|---|
| `ntsc_copy_to_frame` | 407 ms (187 ms with `--no-dropout`) |
| `ntsc_decode_single_field` | 193 ms |
| `ntsc_combine_still_and_moving` | 4.5 ms |
| `ntsc_decode_two_fields` | 3.7 ms |
| `ntsc_detect_color_burst_phase` | 0.3 ms |

Two shaders account for essentially all of it, and both for the same reason: they re-read
global memory many times over where the data they need spans only a few distinct addresses.

`ntsc_copy_to_frame` is a 24-tap de-emphasis FIR that calls `is_dropout()` — itself a 9-tap
scan — inside *every* tap, and on a flagged sample additionally searches outward over up to
eight lines. That is roughly 216 global reads per pixel in the clean case and up to about
1900 in a dropout. Dropout concealment alone costs 219 ms per field, which is what the
`--no-dropout` column isolates.

`ntsc_decode_single_field` recomputes `chroma_sample()`, three global reads, once per tap of
a 19-tap loop: about 95 reads per pixel across only some 21 distinct columns.

The scale of the waste shows up in the achieved rates. These 2D decode shaders manage
roughly 50–60 MMAC/s. The linear RF demodulation FIRs — which stage their input through
shared memory before filtering — reach about 835 MMAC/s on the same GPU. The decode shaders
are therefore around 15× off, purely from untiled redundant loads.

This is the one clear optimisation lead in the whole investigation, and it is not enough:
recovering all 15× still leaves the decode stage roughly 2× short, with the demodulation
stage separately 4–6× over budget on top.

## Command buffer recording

A separate and unrelated cost: V3DV spends **44–52 ms of CPU per block** merely recording
the demodulation command buffer, against 0.14 ms for llvmpipe. Broken down, essentially all
of it falls in the six `enqueueComputeShader` calls, each of which inserts a pipeline
barrier — and on V3DV a barrier ends the current job and begins another.

It is real, it is probably reducible, and it is dwarfed by everything above.

## Changes in this branch

The commit accompanying this document contains the minimum needed to get museld onto V3D at
all. Each change is marked `EXPERIMENT (pi5-investigation)` in the source. They are
diagnostic scaffolding, not a proposed Pi 5 port:

- `VulkanManager` — treat `shaderFloat16`, `shaderInt16` and `shaderInt8` as optional rather
  than required, and enable at device creation only those the device actually has.
- `ComputeShader` — clamp the default linear workgroup size to the device's
  `maxComputeWorkGroupInvocations` instead of assuming 1024.
- `VulkanMemoryObject` — accept uncached host-visible memory as a last resort, since V3D
  offers no `eHostCached` type at all.
- `MuseRfDemodulator` — break the recorded-time figure down by phase, which is how the
  command buffer recording cost above was located.

## If this is revisited

The kernel used here, 6.1.69, dates from December 2023 and is old for a Pi 5; current
Raspberry Pi OS carries 6.12 and Mesa 25.x. That is worth re-testing for certainty, and it
might well dent the command buffer recording overhead. It will not move a 25× gap.

Closing that gap needs an algorithmic change rather than driver or shader tuning — a
cheaper concealment strategy and shared-memory tiling in the 2D decode shaders would be the
places to start, but the demodulation stage would still have to be reconsidered separately.
