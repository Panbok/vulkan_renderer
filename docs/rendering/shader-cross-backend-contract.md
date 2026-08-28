---
status: partial
updated: 2026-08-28
authority: design
---

# Shader cross-backend contract

**Reviewed parity state: ALIGNED.** Every entry in this initial ledger has a
Metal and a Vulkan production source, a host or reflection witness where an ABI
is involved, and runtime evidence from both backends. Shader contracts not
listed here are **unreviewed**, not implicitly aligned. The document is
`partial` because this is deliberately not a complete shader inventory.

This is the durable record of values that must agree across VKR's selected
backends. Code remains implementation authority. When code and this document
disagree, the code describes what ran and this document must become
**UNALIGNED** until both backends are corrected and revalidated.

## 1. Parity states and evidence

Each reviewed contract has one of two states:

- **ALIGNED** — both production shader paths implement the recorded contract;
  host layout and compiled-shader reflection are checked where applicable; CPU
  reference tests pin deterministic math; and focused Release snapshots prove
  that both compiled backends execute the path with real data.
- **UNALIGNED** — either backend differs, or source/layout/runtime evidence is
  missing on one side. The entry must name the missing side and gate. If any
  reviewed entry is unaligned, the document-level parity state is also
  **UNALIGNED**.

A shared source file is a strong source invariant, but is not by itself proof
that both compiled backends include or execute it. Snapshot values must record
the case, backend/device, configuration, and report digest. Artifact directories
are regenerable and are not part of this contract.

Native resource roots do not need identical byte layouts. Metal stores 64-bit
resource references while Vulkan stores 32-bit bindless indices. Such roots are
aligned when their semantic fields map to the same algorithm and each native
layout is independently validated.

Constants, field types/order, declared offsets, dispatch shapes, and algorithm
steps require exact parity. Rendered values require a documented, case-specific
numeric comparison because native texture compression and floating-point paths
need not be bit-identical. An observed difference is not a reusable tolerance;
changing an acceptance envelope requires new evidence and owner review.

## 2. Production source topology

| Contract | Shared math | Metal production path | Vulkan production path | State |
| --- | --- | --- | --- | --- |
| Tangent-space normal decode | `shaders/shared/normal_map_kernel.slangh` | `metal/msl/world/default.metal`; deferred and transmission call sites in `metal/msl/world/gpu_draws.metal` | `vulkan/slang/world/default.slang`; deferred and transmission share `vulkan/slang/world/deferred.slang` | **ALIGNED** |
| Automatic exposure | `shaders/shared/exposure_kernel.slangh` | `metal/msl/post/exposure.metal` | `vulkan/slang/post/exposure.slang` | **ALIGNED** |
| Bloom | `shaders/shared/exposure_kernel.slangh`, then `shaders/shared/bloom_kernel.slangh` | `metal/msl/post/bloom.metal` | `vulkan/slang/post/bloom.slang` | **ALIGNED** |
| GTAO | `shaders/shared/gtao_kernel.slangh` | `metal/msl/post/gtao.metal` | `vulkan/slang/post/gtao.slang` | **ALIGNED** |

Paths in this document are relative to `lib/src/renderer/`. Vulkan includes the
shared headers through Slang. The Metal library concatenates the same headers
before its native MSL sources.

## 3. Verified shader data layouts

All offsets and sizes are bytes. `float` and `uint` below are 32-bit. The
4-byte scalar sequences have no implicit vector padding. Native roots use
`VKR_SIMD_ALIGN`, which is 16 bytes on the host.

### 3.1 Shared parameter and state blocks

| Shader / host pair | Member layout | Size | Required alignment | State |
| --- | --- | ---: | ---: | --- |
| `VkrExposureMetering` / `VkrExposureGpuMetering` | 14 `float` fields at offsets `0..52`; `uint bin_count` at `56`; `uint history_valid` at `60` | 64 | 4; containing roots are 16-aligned | **ALIGNED** |
| `VkrExposureState` / `VkrExposureGpuState` | 6 `float` fields at offsets `0..20`; `uint accepted_texel_count` at `24`; `uint reset_reasons` at `28` | 32 | 4 | **ALIGNED** |
| exposure histogram | `uint bins[256]`, offsets `0..1020` | 1024 | 4 | **ALIGNED** |
| `VkrBloomParams` / `VkrBloomGpuParams` | `float threshold`, `knee`, `knee_denominator`, `firefly_clamp`, `intensity` at `0,4,8,12,16`; three reserved floats at `20,24,28` | 32 | 4; containing roots are 16-aligned | **ALIGNED** |
| `VkrGtaoParams` / `VkrGtaoGpuParams` | `float4x4 view` at `0`; four `uint` values at `64..76`; 24 `float` values at `80..172`; four `uint` values at `176..188` | 192 | 16 | **ALIGNED** |

The GTAO field-level witnesses cover every member. Important boundaries are
`projection_m22` at `88`, `projection_m32` at `96`, `projection_m00` at `104`,
`projection_m02` at `112`, `effect_radius` at `128`, `final_value_power` at
`156`, and `slice_count` at `176`. Metal validates the 192-byte, 16-aligned
record and every field against native shader reflection. Vulkan validates every
field and the total size against SPIR-V reflection at pipeline creation.

### 3.2 Backend-native roots

| Contract | Metal host/shader root | Vulkan host/shader root | Semantic mapping | State |
| --- | --- | --- | --- | --- |
| Exposure | `VkrMetalPacketExposureRoot`: 112 bytes, align 16; metering at `48` | `VkrVkExposureRoot` / `VkrVulkanExposureRoot`: 112 bytes, align 16; metering at `40` | histogram, current/previous state, sampled HDR source, extent, reset reasons, metering | **ALIGNED** |
| Bloom | `VkrMetalPacketBloomRoot`: 80 bytes, align 16; params at `40` | `VkrVkBloomRoot` / `VkrVulkanBloomRoot`: 64 bytes, align 16; params at `32` | source, coarse source, destination, filter/destination extents, params | **ALIGNED** |
| GTAO depth | `VkrMetalPacketGtaoDepthRoot`: 224 bytes, align 16; params at `0` | shared `VkrVkGtaoRoot` / `VkrVulkanGtaoRoot`: 240 bytes, align 16; params at `0` | source/destination plus extents | **ALIGNED** |
| GTAO evaluate | `VkrMetalPacketGtaoEvaluateRoot`: 256 bytes, align 16; params at `0` | shared `VkrVkGtaoRoot` / `VkrVulkanGtaoRoot`: 240 bytes, align 16; resources begin at `192` | visibility/depth/normal inputs, destination, edges, extents, point-clamp sampling | **ALIGNED** |
| GTAO denoise | `VkrMetalPacketGtaoDenoiseRoot`: 240 bytes, align 16; params at `0` | shared `VkrVkGtaoRoot` / `VkrVulkanGtaoRoot`: 240 bytes, align 16; resources begin at `192` | source, edges, destination, extents | **ALIGNED** |

Metal root sizes, alignments, and offsets are checked by
`metal/vkr_metal_packet_abi.c` against native library reflection. Vulkan's GTAO
root and nested parameters are checked by `vkr_vulkan_pipelines.c` against
SPIR-V reflection; all Vulkan root sizes and selected boundary offsets are also
compile-time assertions in `vulkan/vkr_vulkan_internal.h`. Exposure and bloom
parameter sizes are compile-time assertions in `vkr_exposure.h` and
`vkr_bloom.h` and are exercised by both backend snapshots.

## 4. Verified algorithms and values

### 4.1 Tangent-space normal decode — ALIGNED

Every production material path performs the same positive-hemisphere decode:

1. `xy = encoded.xy * 2 - 1`;
2. multiply `xy` by normal strength;
3. flip green with `xy.y = -xy.y`; and
4. reconstruct `z = sqrt(saturate(1 - dot(xy, xy)))`.

This makes BC5 and EAC RG11 two-channel normal maps equivalent to widened ASTC
or RGBA targets. The sampled blue channel is deliberately irrelevant.

### 4.2 Automatic exposure — ALIGNED

| Value | Verified contract |
| --- | --- |
| luminance | scene-linear Rec. 709: `(0.2126, 0.7152, 0.0722)` |
| histogram | 256 log2-luminance bins; out-of-window values saturate to edge bins |
| accepted sample | finite, positive, and at least `0.0001` luminance |
| default log window | `[-10, +10]` |
| default percentile window | `[0.50, 0.95]`, with fractional-bin retention |
| middle gray and EV clamp | `0.18`; `[-8, +8] EV` |
| manual fallback multiplier | `0.30` |
| adaptation | bounded linear EV step; brighten `3 EV/s`, darken `1 EV/s`; frame delta capped at `0.25 s`; invalid history snaps to target |
| dispatch shape | histogram `16x16`; clear and resolve `256x1`; resolve uses an inclusive Hillis-Steele scan |

The target is `log2(middle_gray) - average_log_luminance + compensation`, then
clamped. An empty retained window holds the previous EV. Exposure meters the
pre-bloom HDR image, so bloom cannot feed back into metering.

### 4.3 Bloom — ALIGNED

| Value | Verified contract |
| --- | --- |
| packet defaults | threshold `1.0`, knee `0.5`, intensity `0.05` |
| production chain | at most 6 mips of the compile-time bound 8; minimum mip extent 8; half-resolution mip 0 |
| firefly clamp | `32.0`; positive finite ceiling `65504.0` |
| soft-knee denominator | `4 * knee + 0.0001` |
| threshold brightness | maximum RGB component, not luminance |
| Karis weight | `1 / (1 + dot(rgb, Rec.709))` |
| 13-tap weights | center `0.125`; four corners `0.03125`; four axes `0.0625`; four outer bilinear taps `0.125` |
| dispatch shape | `8x8` for every stage |

NaN, negative infinity, and non-positive components sanitize to zero; positive
infinity and large positive values clamp to the firefly ceiling. Prefilter
applies the Karis-weighted 13-tap reduction and
soft threshold. Later levels use the same 13-tap kernel, then the reverse chain
upsamples into a separate accumulation image. Combine adds
`bloom * intensity` to the original HDR source. The four-tap box alternative is
implemented on both sides but is not the production default.

The aligned claim here covers constants, filtering/topology, ABI, and
non-degenerate execution on both backends. It does not claim bit-identical
cross-device bloom pixels or an accepted final-color baseline.

### 4.4 GTAO — ALIGNED

| Value | Verified contract |
| --- | --- |
| frame defaults | radius `0.5`, final visibility power `2.2` |
| quality defaults | 5 depth mips; 3 slices x 3 steps |
| sampling | radius multiplier `1.457`; falloff range `0.615`; distribution power `2.0`; depth-mip offset `3.30` |
| denoise | edge-aware 3x3; beta `1.2`; diagonal edge coefficient `0.425` |
| noise | 64x64 Hilbert domain; temporal index modulo 64 |
| depth | right-handed view space; maximum view depth `65504.0` |
| visibility | horizon result divided by slice count, clamped, raised to final power, then floored at `0.03` |
| dispatch shape | `8x8` for prefilter, mip reduction, evaluate, and denoise |

Both backends use the shared projection reconstruction, farthest-depth weighted
mip filter, edge packing, Hilbert noise, sample distribution, falloff, horizon
integration, final visibility, and diagonal denoise weight. GTAO multiplies
indirect diffuse only.

## 5. Cross-backend runtime witnesses

All rows are Release snapshots with validation layers disabled. These are
correctness witnesses, not performance comparisons or accepted golden images.

| Contract | Metal witness | Vulkan witness | Recorded real values |
| --- | --- | --- | --- |
| Normal decode, exposure, GTAO on | M1 Pro, `mac_bistro_exposure_parity`, `sha256:66737ad0e780cb142756fb687f7b1e4a5a1d0f3683b156dcbc246b5de2f748e2` | RX 6700 XT, matched Bistro case, `sha256:dd4dd35397fb2b83d73e571c43d44a7a3431068c4a2fd2695d2e7b31e2c23a72` | average log luminance `-3.309451` / `-3.308282`; EV `+0.835520` / `+0.834351`; multiplier `1.784500` / `1.783055` |
| Exposure, GTAO off | M1 Pro, `mac_bistro_exposure_parity_gtao_off`, `sha256:87ef98262d33a9e6c9db87a78a779c4579ece6fd2e7402003a545479ccfdad50` | RX 6700 XT, matched Bistro case, `sha256:3e497612b394ead71cbb876c669cec7db4ae44230db8db0eab3a5ce50ce235b1` | average log luminance `-3.118539` / `-3.115359`; EV `+0.644608` / `+0.641427`; multiplier `1.563315` / `1.559872` |
| Bloom execution | M1 Pro, `smoke.bloom.static`, `sha256:333b97da86b2e226c7a9528ff579888e80c70cb2034d4655a51acaea07151659` | RX 6700 XT, `smoke.bloom.static`, `sha256:3af307a224ca770abe8a9efd2e81b8d6e1411a4acdbcf780299f606604a4c260` | Metal's prefilter/result are non-degenerate; Vulkan's prefilter, result, combined HDR, and final color have distinct digests |

For the GTAO-on pair, only 3 of 1,920,000 depth-mask pixels differ. Across
1,919,654 common foreground pixels, decoded normals have mean dot `0.999080`,
component MAE `0.007001`, and `0.0273%` negative dots. Mean raw and denoised
GTAO visibility differ by `0.000411` and `0.000406`. Exposure differs by
`0.001169 EV` with GTAO on and `0.003181 EV` with it off; GTAO attenuation
differs by `0.002012 EV`.

Deterministic CPU mirrors in `vkr_exposure.c`, `vkr_bloom.c`, and `vkr_gtao.c`
pin the shared math. `build_test.bat` passed on Windows after the fixes recorded
above. The authoritative evidence chain and reproduction context remain in
[Windows Vulkan post-effect parity investigation](windows-vulkan-post-effect-parity-investigation.md)
and [Post-exposure, bloom, and ambient-occlusion spec](post-exposure-bloom-and-ambient-occlusion-spec.md).

## 6. Maintenance rule

Shader work must use `.codex/skills/vkr-shaders/SKILL.md`. A change to a listed
value, structure, binding, entry point, or algorithm updates this ledger in the
same change. New facts enter as **UNALIGNED** until both production backends and
their evidence gates pass. Never preserve an **ALIGNED** label by omitting the
backend that has not been checked.
