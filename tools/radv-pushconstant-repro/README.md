# RADV repro: divergently indexed push constant array reads return the wrong element

Minimal reproducer for a RADV miscompilation found while debugging garbled OSD text in
museld on a Renoir iGPU. Build and run with `make run` (needs `glslc`, g++, libvulkan-dev).

The text below is written as a Mesa issue report.

---

## Title

radv: loads from a push constant array with a divergent index return the first lane's
element for all lanes (Renoir)

## Environment

- GPU: AMD Radeon Graphics (RADV RENOIR) — Ryzen 5700U "Lucienne" iGPU, deviceID 0x164c
- Mesa 25.2.8 (Ubuntu 25.10 package `mesa-vulkan-drivers 25.2.8-0ubuntu0.25.10.2`)
- Kernel 6.17.0-41-generic, x86_64
- glslc from shaderc 2025.2

Control: the same binary and SPIR-V pass on lavapipe from the *same* Mesa build
(`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./repro repro.spv` → PASS), so the
shader, the host program and the common NIR path are all fine and this is RADV-specific.
The same SPIR-V also passes on MoltenVK (Apple M2, portability flags added to the host
program for that run). The repro is clean under VK_LAYER_KHRONOS_validation 1.4.350.

## Description

In a compute shader, reading an element of a push constant array with an index that
differs per invocation (divergent) returns the element addressed by the first active
lane of the wavefront for *all* lanes, instead of each lane's own element. The identical
access pattern through an SSBO returns correct results.

Observed on both compiler backends (default ACO and `RADV_DEBUG=llvm`, LLVM 20.1.8), while
lavapipe from the same Mesa build is correct. That combination points at RADV's own NIR
lowering of `nir_load_push_constant` — the part ACO and LLVM share but lavapipe does not —
assuming the offset is uniform, rather than at either backend or at common NIR.

No extensions or optional features are involved: core Vulkan 1.1, plain `uint` array,
fixed `local_size_x = 64`, one `vkCmdDispatch(1, 1, 1)`. The same wrong results are also
seen with 16-bit element types (`uint16_t` + `storagePushConstant16`) and with the
workgroup size set via specialization constants, so it is not specific to those either.

Shader (`repro.comp`):

```glsl
#version 450

layout(local_size_x = 64) in;

layout(push_constant) uniform PushConstants {
    uint pc_data[32];
};

layout(set = 0, binding = 0) buffer readonly In { uint ssbo_data[32]; };
layout(set = 0, binding = 1) buffer writeonly Out { uint outv[]; };

void main() {
    uint x = gl_GlobalInvocationID.x;
    outv[x] = pc_data[x / 2];       // divergent index into push constants
    outv[64 + x] = ssbo_data[x / 2]; // same access pattern via SSBO (control)
}
```

The host program fills both `pc_data` and `ssbo_data` with `0x1000 + i`, dispatches one
64-thread workgroup, and compares the output against the expected `0x1000 + x / 2`.

## Expected

`outv[x] == 0x1000 + x / 2` for all 64 threads. This is what the SSBO control column
produces on the same dispatch, and what lavapipe produces for the push constant column.

## Actual

Threads 0 and 1 are correct; every other thread receives `pc_data[0]`:

```
device: AMD Radeon Graphics (RADV RENOIR)
thread  2: pc_data[ 1] = 0x1000, expected 0x1001   (ssbo control = 0x1001)
thread  3: pc_data[ 1] = 0x1000, expected 0x1001   (ssbo control = 0x1001)
thread  4: pc_data[ 2] = 0x1000, expected 0x1002   (ssbo control = 0x1002)
...
thread 62: pc_data[31] = 0x1000, expected 0x101f   (ssbo control = 0x101f)
thread 63: pc_data[31] = 0x1000, expected 0x101f   (ssbo control = 0x101f)
FAIL: 62 wrong values
```

In a variant with a 32x2 workgroup (set via specialization constants) the replication
granularity was 32 lanes: lanes 0–31 all read the element lane 0 addressed, lanes 32–63
all read the element lane 32 addressed. In every case the load appears to be scalarized
using the first active lane's offset.

## How this was found

museld (a laserdisc player) renders on-screen text with a compute shader that looks up
glyph indices in a push constant array indexed by a pixel-derived character position.
On this GPU the text is rendered with wrong characters whenever a wavefront spans more
than one array element; the corruption is deterministic and disappears when the same
data is read from an SSBO instead.
