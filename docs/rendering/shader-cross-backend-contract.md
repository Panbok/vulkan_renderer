---
status: partial
updated: 2026-08-29
authority: design
---

# Shader cross-backend contract

**Reviewed parity state: UNALIGNED.** The ledger accounts for every source under
`shaders/shared/`, `shaders/metal/`, and `shaders/vulkan/` as of 2026-08-29.
The original normal, exposure, bloom, and GTAO contracts remain **ALIGNED**.
The newly inventoried contracts are **UNALIGNED** where production sources or
dispatch rules differ, where compiled ABI witnesses are incomplete, or where
matched native runtime evidence is missing. The document remains `partial`
until those gates close.

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
| Packed geometry and GPU draw compaction | `shaders/shared/gpu_draw.slangh` for both Slang libraries | Native deferred/ICB path mirrors the records and decode in `metal/msl/common/draw.metalh` and compacts in `metal/msl/world/gpu_draws.metal` | `vulkan/slang/common/resources.slangh`, `common/vertex.slangh`, and `world/deferred.slang` | **UNALIGNED**: overflow policy differs and the native MSL mirror is not shared source |
| Cascaded-shadow receiver | `shaders/shared/shadow_kernel.slangh` | `metal/msl/shadow/sampling.metalh`, used by forward and deferred paths | `vulkan/slang/world/default.slang`, reused by `world/deferred.slang` | **UNALIGNED**: sources agree, but the current matched native snapshot gate is missing |
| IBL baking and sampling | None | `metal/msl/ibl/*.metal*`, `metal/msl/world/lighting.metalh` | `vulkan/slang/ibl/*.slang*`, `vulkan/slang/world/default.slang` | **UNALIGNED**: no direct bake-output snapshot pair or compiled-root reflection pair |
| Visibility, deferred resolve, lighting, temporal resolve, transmission, and picking | Packed rows and normal decode above | `metal/msl/world/gpu_draws.metal` | `vulkan/slang/world/deferred.slang`, `vulkan/slang/picking/default.slang` | **UNALIGNED**: source parity is broad, but draw overflow differs and matched runtime/ABI gates remain open |
| Tonemap and FXAA | Exposure state above | `metal/msl/post/tonemap.metal` | `vulkan/slang/post/default.slang`, `post/tonemap.slangh` | **UNALIGNED**: the tonemap-disabled path differs |
| Text color and picking | None | `metal/msl/text/default.metal` | `vulkan/slang/text/default.slang` | **UNALIGNED**: sources agree semantically, but no matched native snapshot pair is recorded |

Paths in this document are relative to `lib/src/renderer/`. Vulkan includes the
shared headers through Slang. The Metal library concatenates the same headers
before its native MSL sources. `gpu_draw.slangh` is the exception: it is
compiled into the Metal Slang library, while the native Metal deferred library
keeps a second MSL definition and decode. That mirror is a parity obligation,
not shared ownership.

### 2.1 Requested source inventory

This table makes the audit boundary explicit. An aggregation or declaration
file is covered by the contract sections that consume it; it is not a separate
algorithm.

| Source | Contract recorded here |
| --- | --- |
| `shaders/shared/README.md` | Shared-source ownership and the native-MSL GPU-row exception in section 2 |
| `shaders/shared/normal_map_kernel.slangh` | Normal decode in section 4.1 |
| `shaders/shared/exposure_kernel.slangh` | Exposure in section 4.2 |
| `shaders/shared/bloom_kernel.slangh` | Bloom in section 4.3 |
| `shaders/shared/gtao_kernel.slangh` | GTAO in section 4.4 |
| `shaders/shared/gpu_draw.slangh` | Packed geometry and draw rows in sections 3.3 and 4.5 |
| `shaders/shared/shadow_kernel.slangh` | Progressive Poisson table in section 4.6 |
| `shaders/vulkan/slang/library.slang` | Aggregates every Vulkan source below plus the shared headers |
| `shaders/vulkan/slang/common/resources.slangh` | Bindless resource topology and packet/GPU records in section 3.3 |
| `shaders/vulkan/slang/common/vertex.slangh` | Packed/static vertex fetch in section 4.5 |
| `shaders/vulkan/slang/common/fullscreen.slang` | Fullscreen triangle in section 4.9 |
| `shaders/vulkan/slang/ibl/common.slangh` | Hammersley, cube-face, and GGX helpers in section 4.7 |
| `shaders/vulkan/slang/ibl/default.slang` | IBL bake kernels in section 4.7 |
| `shaders/vulkan/slang/picking/default.slang` | Forward object-ID output in section 4.8 |
| `shaders/vulkan/slang/post/exposure.slang` | Exposure dispatches in section 4.2 |
| `shaders/vulkan/slang/post/bloom.slang` | Bloom dispatches in section 4.3 |
| `shaders/vulkan/slang/post/gtao.slang` | GTAO dispatches in section 4.4 |
| `shaders/vulkan/slang/post/tonemap.slangh` | ACES fit in section 4.9 |
| `shaders/vulkan/slang/post/default.slang` | Exposure application, FXAA, and fullscreen output in section 4.9 |
| `shaders/vulkan/slang/text/default.slang` | Bitmap/MTSDF text and text picking in section 4.10 |
| `shaders/vulkan/slang/world/default.slang` | Forward PBR, shadows, IBL, and temporal blend in sections 4.6-4.8 |
| `shaders/vulkan/slang/world/deferred.slang` | GPU draws, visibility, resolve, lighting, temporal, HZB/SDSM, transmission, and picking in sections 4.5-4.8 |

### 2.2 Metal production inventory

The Metal tree has 19 production files and 43 distinct entry points: 40 native
MSL entry points and three Slang entry points compiled to Metal. `lib/CMakeLists.txt`
builds the native library by concatenating shared math first, then Metal headers
and implementation files. `metal/slang/library.slang` is compiled separately.

| Domain | Metal files and entry points | Vulkan counterpart | Review state |
| --- | --- | --- | --- |
| Packed geometry and draw vertices | `metal/slang/common/draw.slangh`, `metal/slang/world/default.slang`, and `metal/slang/picking/world.slang`; `vkr_metal_packet_vertex`, `vkr_metal_packet_temporal_vertex`, `vkr_metal_packet_picking_fragment` | `vulkan/slang/common/resources.slangh`, `common/vertex.slangh`, `world/default.slang`, and `picking/default.slang` | **UNALIGNED**: source counterpart exists; matched Vulkan runtime gate missing |
| Native draw/resource declarations | `metal/msl/common/draw.metalh`; no entry point; this native MSL path mirrors rather than includes `shaders/shared/gpu_draw.slangh` | Vulkan resource and vertex headers above include the shared Slang records | **UNALIGNED**: native layouts are independently defined; current matched Vulkan reflection witness missing |
| Fullscreen triangle | `metal/msl/common/fullscreen.metalh`; helper only | `vulkan/slang/common/fullscreen.slang` | **UNALIGNED**: source counterpart exists; matched Vulkan runtime gate missing |
| Lighting and shadow sampling | `metal/msl/world/lighting.metalh`, `metal/msl/shadow/sampling.metalh`; shared progressive PCF comes from `shaders/shared/shadow_kernel.slangh` | `vulkan/slang/world/default.slang` and the same shared shadow kernel | **UNALIGNED**: source counterpart exists; matched native snapshots for the current source are missing |
| GPU draw generation, visibility, resolve, deferred lighting, temporal history, transmission, picking, HZB, and SDSM | `metal/msl/world/gpu_draws.metal`; 18 entry points from `vkr_metal_packet_vbuffer_fragment` through `vkr_metal_packet_sdsm_reduce` | `vulkan/slang/world/deferred.slang` | **UNALIGNED**: dispatch differences and missing matched Vulkan evidence; Metal transmission compaction has no Vulkan entry point |
| Forward world shading and temporal MRT output | `metal/msl/world/default.metal`; `vkr_metal_packet_opaque_fragment`, `vkr_metal_packet_temporal_blend_fragment` | `vulkan/slang/world/default.slang` | **UNALIGNED**: source counterpart exists; matched Vulkan runtime gate missing |
| IBL baking | `metal/msl/ibl/common.metalh` plus three IBL `.metal` files; equirect, irradiance, and prefilter entry points | `vulkan/slang/ibl/default.slang` | **UNALIGNED**: source counterpart exists; matched IBL snapshots and native validation missing |
| Text and text picking | `metal/msl/text/default.metal`; vertex, color-fragment, and picking-fragment entry points | `vulkan/slang/text/default.slang` | **UNALIGNED**: source counterpart exists; matched text snapshots and native validation missing |
| Tonemap and FXAA | `metal/msl/post/tonemap.metal`; fullscreen vertex and tonemap fragment | `vulkan/slang/post/default.slang` and `post/tonemap.slangh` | **UNALIGNED**: source counterpart exists; matched final-color evidence missing |
| Exposure, bloom, and GTAO | the three files listed in the original topology table; 12 compute entry points | the three Vulkan post-effect files listed above | **ALIGNED** |

The broad `gpu_draws.metal` row contains four visibility fragments and fourteen
compute entry points. It owns classify, prefix, ICB encode, picking resolve,
temporal transform, G-buffer resolve, deferred lighting, temporal resolve,
transmission shade/compact/finalize/coverage, HZB build, and SDSM reduction.

### 2.3 Metal binding and dispatch rules

Metal passes native GPU addresses and resource references as 64-bit fields.
The shader roots reconstruct device pointers, textures, samplers, ICBs, and
indirect-dispatch argument buffers from those values. Vulkan maps the same
semantic resources through buffer references and 32-bit bindless indices.

Pipeline creation checks the relevant root against native Metal reflection
before the pipeline can be used. Compute roots and the native visibility vertex
root bind at buffer index 0. Slang draw roots, forward fragments, text, and
tonemap bind at index 1. The transmission peel root binds at index 2. Reflection
also walks nested draw, frame, geometry, visible-row, and packed-vertex records.
The host transposes draw matrices only for the Slang-to-Metal vertex path through
`vkr_metal_packet_slang_draw_matrix()`.

| Work | Metal dispatch | Vulkan dispatch | State |
| --- | --- | --- | --- |
| GPU classify and encode | two-dimensional grid `candidate_count x view_count`; `64x1x1` threadgroups | flattened candidate/view grid; `64x1x1` shader group | **UNALIGNED** dispatch shape |
| GPU prefix | one grid thread per view; one threadgroup whose width is `view_count` | `1x1x1` shader group dispatched once per view | **UNALIGNED** dispatch shape |
| Temporal transform | one thread per instance; `64x1x1` | one thread per instance; `64x1x1` | source-aligned; runtime gate missing |
| G-buffer, deferred lighting, temporal resolve, HZB, transmission shade/coverage, bloom, and GTAO | extent grid; `8x8x1` | extent grid; `8x8x1` | source-aligned where both entry points exist; runtime gate missing for newly reviewed rows |
| Transmission compaction | extent grid at `8x8x1`, then one `1x1x1` finalize dispatch that writes indirect threadgroups for a `64x1x1` compact shade dispatch | no compact/finalize entry points; Vulkan shades the extent at `8x8x1` | **UNALIGNED** implementation |
| Picking resolve | one `1x1x1` dispatch | one `1x1x1` dispatch | source-aligned; runtime gate missing |
| SDSM reduction | extent grid; `16x16x1` | extent grid; `8x8x1` | **UNALIGNED** dispatch shape |
| IBL bake | cube extent by six faces; `8x8x1` | cube extent by six faces; `8x8x1` | source-aligned; runtime gate missing |

The `p20_metal_state_matrix` and `p20_vulkan_state_matrix` cases cannot close
these rows as written. They differ in skybox state, warm-up/capture frames,
captured channels, and assertions, so their workload and policy fingerprints do
not match. Closing a row requires Release snapshots with identical scene and
frame state, matching workload and policy fingerprints, comparable channels,
and a separate focused native validation run on each backend.

## 3. Shader data layouts

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

### 3.3 Packed rows and Vulkan roots — UNALIGNED

The shared packed records have these host contracts:

| Record | Size | Alignment | Important offsets |
| --- | ---: | ---: | --- |
| `VkrPackedStaticVertex` | 32 | 4 | `words[8]` at `0` |
| `VkrGpuGeometryDecodeRecord` | 32 | 4 | position bias `0`, flags `12`, position scale `16` |
| `VkrGpuGeometryRow` | 48 | 8 | vertex/index addresses `0/8`, first vertex/index `16/20`, layout `28`, generation `32`, decode address `40` |
| `VkrGpuCandidateDrawRow` | 48 | 16 | geometry/material/instance `0/4/8`, index range `12/16`, vertex offset `20`, bucket/flags `24/28`, sphere `32` |
| `VkrGpuVisibleDrawRow` | 32 | 4 | the first eight candidate fields at offsets `0..28` |

`vkr_gpu_abi.c` validates every listed host offset. Vulkan pipeline creation
reflects packed vertices, geometry rows, visible rows, the 144-byte material
row, the 48-byte draw root, and selected fields plus the 480-byte extent of the
frame root. It does not reflect the candidate row or every deferred compute
root. Metal's native reflection covers its own roots, but the native deferred
library declares the packed records independently of `gpu_draw.slangh`.

All Vulkan shaders share a 16-byte push constant containing the root device
address at `0`, material index at `8`, and flags at `12`. Sampled texture aliases
share set 0 binding 0, sampler and comparison-sampler aliases share set 0
binding 1, and storage-image aliases share set 1 binding 0. The host pins the
following Vulkan root sizes, but only the packet and GTAO roots have the
field-level SPIR-V reflection described above:

| Root | Bytes | Root | Bytes |
| --- | ---: | --- | ---: |
| cull | 176 | raster | 48 |
| temporal transform | 32 | G-buffer resolve | 352 |
| temporal resolve | 128 | deferred lighting | 128 |
| HZB | 48 | SDSM | 32 |
| picking | 64 | transmission shade | 416 |
| transmission coverage | 32 | IBL bake | 32 |
| packet frame | 480 | packet draw | 48 |
| packet utility | 544 |  |  |

This contract is **UNALIGNED** until the native MSL duplicate is either derived
from the shared declaration or guarded by equivalent field-level reflection,
and the candidate and deferred roots receive compiled-shader witnesses on both
backends. Existing static assertions prevent host-only size drift but do not
prove the compiled layouts.

### 3.4 Complete Metal reflection manifest

The Metal manifest has 33 reflected records. This table records their native
byte sizes; `metal/vkr_metal_packet_abi.c` remains the field-name and offset
authority. Pipeline creation compares every manifest field offset, record size,
and required alignment with native Metal reflection. It also checks nested
records reached through draw and frame pointers.

| Group | Reflected Metal records and byte sizes |
| --- | --- |
| Geometry and scene rows | vertex `64`; instance `80`; material `176`; text vertex `32`; IBL probe `64`; shadow cascade `96` |
| Draw roots | vertex draw `48`; temporal vertex draw `48`; draw `48`; frame `464` |
| Presentation and bake roots | tonemap `32`; equirect `32`; irradiance `32`; prefilter `32`; text `176` |
| Visibility roots | GPU draw `192`; transmission peel `16`; temporal transform `32`; G-buffer resolve `352` |
| Shared post-effect records | GTAO params `192`; GTAO depth `224`; GTAO evaluate `256`; GTAO denoise `240`; exposure `112`; bloom `80` |
| Lighting, temporal, and transmission roots | deferred lighting `160`; temporal resolve `208`; transmission shade `448`; transmission coverage `32`; transmission compact `80` |
| Utility roots | picking resolve `128`; HZB build `48`; SDSM `32` |

The host gives root uploads 256-byte alignment and reserves 512 bytes for each
draw-root ring cell. Other shader-visible size assertions cover the 32-byte
packed static vertex, 32-byte geometry decode record, 80-byte GPU draw
compaction state, 112-byte transmission diagnostics record, and 16-byte SDSM
state. The frame root keeps the retired 64-bit BRDF-LUT slot as named padding at
offset 104 so later fields do not move.

| Metal constant | Value |
| --- | ---: |
| maximum GPU draw views | 5 |
| transmission visibility layers | 4 |
| temporal transform capacity | 32,768 |
| root upload alignment | 256 bytes |
| draw-root ring stride | 512 bytes |

Different native sizes are expected where the resource representation differs.
For example, Metal/Vulkan material rows are `176/144` bytes, frame roots are
`464/480`, deferred-lighting roots are `160/128`, temporal-resolve roots are
`208/128`, transmission-shade roots are `448/416`, and picking-resolve roots
are `128/64`. These size differences do not establish an algorithm mismatch;
each backend must pass its own reflection gate and map the same semantic values.

## 4. Algorithms and values

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

### 4.5 Packed vertices and GPU draw compaction — UNALIGNED

The packed static vertex is eight 32-bit words. Position XYZ are UNORM16 values
in words 0 and 1, with `position_bias + unorm * position_scale` reconstruction.
The remaining high half of word 1 stores tangent sign bit 0. Normal and tangent
are octahedral SNORM16 pairs in words 2 and 3; UV is two IEEE floats in words 4
and 5; color is RGBA8 in word 6. Word 7 is reserved. Both implementations use
the same fold-and-normalize octahedral decode and map tangent bit 0 to `-1`, or
to `+1` when clear.

GPU draw classification and encoding use `64x1` groups. Metal evaluates a
two-dimensional `candidate_count x view_count` grid for at most five views and
runs prefix as one group whose width is `view_count`; Vulkan flattens the
candidate/view grid and uses a `1x1` prefix group per view. The dispatch shapes
are therefore not aligned. The cold packet boundary limits candidates and
visible rows to 262,144. The state buckets are opaque back-face culling, opaque
double-sided, cutout back-face culling, and cutout double-sided.

Metal's encode kernel compacts visible rows and writes indexed-triangle commands
into ICBs. Each command binds the per-view draw root at vertex buffer 0 and
fragment buffer 1.

The overflow rule is not aligned:

- Vulkan assigns each bucket a fixed 65,536-command quarter partition. Prefix
  clamps each bucket independently, preserves the other buckets, and counts the
  dropped rows.
- Metal allows any four-bucket distribution whose total is at most 262,144. If
  the total exceeds that capacity, it sets all four execution counts to zero;
  the whole view is suppressed.

This difference is invisible in ordinary under-capacity scenes. Closing it
requires one common overflow rule and a fixture that covers both a single
bucket above 65,536 with total work below 262,144 and total work above 262,144.
The same fixture must assert emitted ranges, visible rows, indirect commands,
and overflow counts on both native backends.

### 4.6 Cascaded-shadow receiver — UNALIGNED

Both production receivers compile the same 64-point progressive Poisson table.
The supported packet tap counts are prefixes of 1, 4, 9, 16, or 32 points. The
receiver rotates the prefix by a stable light-space texel-cell hash:
`(uint(x) * 73856093) ^ (uint(y) * 19349663)`, reduced to 1,024 angles. The cell
is `floor(origin_texels - uv * map_size)`; changing the subtraction to addition
would make the pattern swim when a cascade re-anchors.

Biases and radius are authored in texels. Normal offset moves the world-space
receiver before projection. Constant plus slope bias is converted by the
selected cascade's texel size and depth span before comparison. Every tap uses
hardware comparison sampling. For 16 or more requested taps, the optional
uniform-region path probes the first nine and evaluates the remainder only for
a penumbra. Cascades cross-fade across the final configured fraction of their
span and the last cascade fades to unshadowed across the configured distance
range.

The shared table and both receiver sources agree. The state remains
**UNALIGNED** because the current shadow-debug snapshot does not contain a
non-background receiver, and no same-case Release Metal/Vulkan comparison is
recorded. The missing gate is a scene with visible lit, shadowed, penumbra,
cascade-blend, and distance-fade regions, captured on both native backends for
tap counts 1, 9, 16 with early-out on and off, and 32.

### 4.7 IBL bake and sampling — UNALIGNED

All bake kernels dispatch `8x8x6`. Equirectangular conversion uses the same
cube-face orientation and longitude/latitude mapping. Diffuse irradiance uses a
Hammersley cosine-hemisphere integral with 128 production samples. Specular
prefilter uses 256 Hammersley GGX samples, PDF-derived sample solid angle, cube
texel solid angle, and mip LOD; roughness at or below `0.001` forces mip 0.
Forward and deferred lighting use the same analytic environment BRDF, local
probe box projection and weighting, and global-probe remainder.

The math agrees for the production nonzero sample counts. A cold-boundary
precondition is material: a zero irradiance sample count produces one sample on
Vulkan but zero on Metal. The host currently submits 128 and the prefilter
count is 256 on both sides. The state remains **UNALIGNED** until the nonzero
precondition is encoded identically, the Vulkan 32-byte bake root has compiled
field-level reflection, and direct equirect, irradiance, and every prefilter-mip
output has a matched Release snapshot pair.

### 4.8 World, deferred, temporal, and picking paths — UNALIGNED

The forward and deferred paths implement the same material model: GGX
distribution and visibility, Schlick Fresnel with
`F90 = saturate(max(F0) * 25)`, roughness clamped to `[0.04, 1]`, at most 128
masked point lights, cascaded directional shadowing, local/global IBL, emissive,
and the packet transmission controls. Normal decode is the aligned contract in
section 4.1.

The Vulkan visibility buffer stores `(visible_index + 1, primitive_id |
front_face_bit)`, reserving bit 31 for front-facing state and bits 0-30 for the
primitive. Metal stores `(visible_index + 1, primitive_id)` and reconstructs
face sign from the homogeneous projected triangle orientation. These native
representations are allowed to differ; the semantic contract is the same
normal orientation and primitive identity at resolve. Metal's opaque visibility
fragment uses early fragment tests; cutout samples base alpha before writing;
transmission peel rejects depth at or in front of the previous layer plus its
epsilon. Metal G-buffer resolve reconstructs perspective-correct barycentrics
in homogeneous clip space, derives texture gradients, decodes the packed
vertex, and writes albedo, specular, octahedral normal, emissive/debug, motion,
and validity outputs.

G-buffer resolve, deferred lighting, TAA, HZB, transmission shade, and
transmission coverage dispatch `8x8` on both backends. Picking resolve is
`1x1`. SDSM is not aligned: Metal dispatches `16x16`, while Vulkan declares
`8x8`. HZB stores the maximum of each clamped 2x2 footprint and Metal includes
odd source-edge texels explicitly. SDSM reduces occupied visibility pixels to
minimum/maximum device depth and count.

Temporal resolve stores scene-linear HDR history with depth, temporal identity,
and primitive identity. A moving camera filters the matching bilinear footprint
and renormalizes accepted samples; a stationary camera samples color history
linearly. The sources agree on history depth tolerance `0.005`, partial
bilinear acceptance threshold `1e-5`, maximum derived reactivity `0.75`,
luminance denominator floor `0.05`, stationary surface threshold `0.01` pixel,
history retention `0.99` stationary or `0.9` moving, and motion attenuation
over 128 pixels. Accepted history is clamped to the current 3x3 color
neighborhood. Metal render modes 7 and 8 expose motion/validity and
acceptance/rejection diagnostics. Picking chooses the selected transmission
layer when present, otherwise opaque visibility, then returns the instance
object ID.

Metal rasterizes four transmission visibility layers. Its compact path scans
each layer at `8x8`, uses SIMD and threadgroup prefix sums to append covered
pixels, then a one-thread finalize kernel writes indirect `64x1` shade dispatch
arguments. Vulkan has the four-layer visibility, fullscreen shade, and coverage
paths, but no compact or finalize kernels. Closing this execution difference
requires either a Vulkan compact implementation with matched output evidence or
an owner-approved contract that permits different dispatch algorithms.

The state remains **UNALIGNED** because the compaction mismatch in section 4.5
feeds every downstream path, most compute roots lack bilateral compiled
reflection, and no one same-case Release comparison currently spans visibility,
G-buffer, lighting, temporal history, transmission, and picking on both native
backends.

### 4.9 Fullscreen output, ACES, and FXAA — UNALIGNED

Both backends use a three-vertex fullscreen triangle. The ACES fit constants
are `(2.51, 0.03, 2.43, 0.59, 0.14)`. FXAA uses square-root Rec. 709 luminance,
edge threshold `max(0.0312, luma_max * 0.125)`, direction reduction factor
`0.03125` with floor `0.0078125`, maximum span 8 pixels, and subpixel strength
`0.75`. In the Metal root, `reserved.x` enables ACES and `reserved.y` enables
FXAA. Those enabled paths agree with Vulkan's fullscreen flags.

The tonemap-disabled path differs. Metal still multiplies by exposure, clamps
negative values, and clamps the result to `[0, 1]`; Vulkan returns the sampled
source RGB unchanged. This contract is **UNALIGNED** until one behavior is
selected and a fixture captures tonemap on/off crossed with FXAA on/off on both
backends.

### 4.10 Text color and picking — UNALIGNED

Bitmap text uses atlas alpha. MTSDF text uses the median of RGB minus `0.5`,
then derivative width `max(fwidth(distance), 1e-6)` and a saturated `+0.5`
coverage reconstruction. Color output preserves glyph RGB and multiplies glyph
alpha by coverage. Metal selects MTSDF when `controls.y > 0.5`; UI flag bit 0
maps through the pixel extent in `controls.zw`. Picking discards coverage at or
below `0.01` and otherwise returns the draw object ID. Screen-space clip
conversion differs in Y between the native APIs to produce the same top-left UI
convention.

The audited sources agree, and the Vulkan text fixture visibly exercises
system, bitmap, and MTSDF text plus picking. The state remains **UNALIGNED**
until the same fixture is captured on Metal and compared with a documented
color and coverage envelope.

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

### 5.1 Metal inventory audit snapshot

The 2026-08-29 audit ran the Release `local.p20.metal.state_matrix` snapshot
twice on an Apple M1 Pro / Apple Metal 4 GPU with `MTL_DEBUG_LAYER` and
`MTL_SHADER_VALIDATION` unset. Both reports identify source revision
`2596af4f876942347ed5d776b0aa56dbf475e597`:

| Run | Report digest | Result |
| --- | --- | --- |
| `20260828T211333.682Z-018123` | `sha256:c6d4159f4f22004d7a8d42dcd8056bbffce2835b3c9cf46e4cde3561aeef340f` | pass; six replay children, each with 15/15 assertions passing |
| `20260828T211725.790Z-018779` | `sha256:e9034a2e50e7d751bd14764ba27acaa72fa8f9106e401b29d395e31b48b1ee9f` | pass; six replay children, each with 15/15 assertions passing |

Each representative child recorded all 48 render-graph pass rows for 30
measured frames with no disabled or omitted row. Both runs reported nine GPU
draw candidates and five visible opaque draws, split `2/1/1/1` across the four
raster buckets. They also reported four transmission candidates and four
visible transmission draws, split `1/1/1/1`. Opaque and transmission overflow,
transmission compact overflow, and invalid G-buffer resolve counts were all
zero. HZB history was valid for all 30 samples.

The snapshot profile is local-only and has no accepted baseline, so both reports
correctly set `authoritative=false` with `profile.local_only` and
`baseline.missing`. A direct comparison of the 13 canonical capture payloads
found six byte-identical channels: final color, visibility IDs, visibility
primitives, transmission visibility IDs, depth, and shadow factor. G-buffer
diffuse/specular/normal, deferred emissive, resolve LOD, shadow cascades, and
shadow depth differed. Those seven captures came from adjacent submitted
frames, so this audit does not treat them as a pixel comparison. The case's
configured limits were maximum absolute delta `2/255`, mean absolute error
`0.1/255`, and failed-pixel ratio `0.001`, but the harness did not evaluate those
limits without a baseline. This verifies non-degenerate Metal execution and
exposes a capture-stability gap; it does not close any newly reviewed
cross-backend row.

Deterministic CPU mirrors in `vkr_exposure.c`, `vkr_bloom.c`, and `vkr_gtao.c`
pin the shared math. `build_test.bat` passed on Windows after the fixes recorded
above. The authoritative evidence chain and reproduction context remain in
[Windows Vulkan post-effect parity investigation](windows-vulkan-post-effect-parity-investigation.md)
and [Post-exposure, bloom, and ambient-occlusion spec](post-exposure-bloom-and-ambient-occlusion-spec.md).

### 5.2 Vulkan audit snapshots

These one-sided checks used the Release Windows Vulkan binary on an AMD Radeon
RX 6700 XT with driver 26.6.3, Clang 20.1.0, and validation layers disabled.
The source revision was `2596af4f876942347ed5d776b0aa56dbf475e597`; the
binary digest was
`sha256:a60aa03a595a0c6a39464c5c5f5af1128bfb82787213ef95eabaff7c5faccd49`.

| Case and configuration | Report digest | What the snapshot establishes |
| --- | --- | --- |
| `local.p20.vulkan.state_matrix`, 640x480 offscreen deferred, frames 32-36 | `sha256:cc57039d55793d75aca538f2a36ea986f54cb5c4fb6c368b1e6446094c71afd6` | Pass; the colored state-matrix geometry is visible. Thirteen distinct captures cover final/scene color, opaque and transmission visibility IDs/primitives, diffuse/specular/normal/emissive G-buffer data, barycentric LOD, depth, and picking. |
| `smoke.sponza.snapshot_debug`, 160x120 hidden-window deferred, frame 1 | `sha256:2efcfe34fd44e54bca6d10ee864f2325a1003ce946af4656970e61f34635e600` | Harness pass and six captures, but visual inspection found only sky/background in the shadow debug views. It does not prove receiver, cascade, or PCF execution. |
| `smoke.text.rendering.snapshot`, 960x540 offscreen automation, frame 1 | `sha256:1d3c6170b3db32f07d57e89ba0cdd221eaa7f6aee0572202d92547b1483c87e9` | Pass; visible system, bitmap, and MTSDF text are non-degenerate. Final color is `sha256:a24fdcef033d033d9f5c9540f4fb57c32a6805fdbe6cd48a6e69305dab0bd952`; picking IDs are `sha256:ebc8eb20d2027f9145306c2a6e0621dec642f5bd93d02efc725e06ef4262472b`. |

All three reports are explicitly non-authoritative because the selected
profiles are local-only and no baseline was supplied. The state-matrix report's
metric assertions are `incomplete`, not passed measurements. These reports are
therefore Vulkan execution witnesses only; they do not advance any newly
inventoried contract to **ALIGNED**.

Reproduction commands:

```sh
build_release/tools/vkr_harness snapshot --case tools/cases/local/p20_vulkan_state_matrix.case.json --profile tools/profiles/local-offscreen.json
build_release/tools/vkr_harness snapshot --case tools/cases/smoke/sponza_snapshot_debug.case.json --profile tools/profiles/local-windowed.json
build_release/tools/vkr_harness snapshot --case tools/cases/smoke/text_rendering_snapshot.case.json --profile tools/profiles/local-offscreen.json
```

### 5.3 Gates that keep the new rows unaligned

The state-matrix rows need the two P20 snapshot cases normalized to identical
scene state, capture frames, channels, and assertions before these commands can
produce comparable reports:

```sh
build_release/tools/vkr_harness snapshot \
  --case tools/cases/local/p20_metal_state_matrix.case.json \
  --profile tools/profiles/local-offscreen.json
build_release/tools/vkr_harness.exe snapshot \
  --case tools/cases/local/p20_vulkan_state_matrix.case.json \
  --profile tools/profiles/local-offscreen.json
```

IBL and text need native reports from the same backend-neutral cases:

```sh
build_release/tools/vkr_harness snapshot \
  --case tools/cases/smoke/hdr_environment_snapshot.case.json \
  --profile tools/profiles/local-offscreen.json
build_release/tools/vkr_harness snapshot \
  --case tools/cases/smoke/text_rendering_snapshot.case.json \
  --profile tools/profiles/local-offscreen.json
```

All newly reviewed render-state rows also require the focused native validation
pair. Metal must run alone:

```sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  build_debug/tools/vkr_harness profile \
  --case tools/cases/local/p20_metal_state_matrix_validation.case.json \
  --profile tools/profiles/local-metal-dual-validation-serial.json
build_debug/tools/vkr_harness.exe profile \
  --case tools/cases/local/p20_vulkan_state_matrix_validation.case.json \
  --profile tools/profiles/validation-windowed.json
```

Each closure record must carry both report digests and the actual comparison
values. The Metal transmission compact/finalize row has an additional missing
Vulkan implementation, so snapshots alone cannot align that dispatch contract.

## 6. Maintenance rule

Shader work must use `.codex/skills/vkr-shaders/SKILL.md`. A change to a listed
value, structure, binding, entry point, or algorithm updates this ledger in the
same change. New facts enter as **UNALIGNED** until both production backends and
their evidence gates pass. Never preserve an **ALIGNED** label by omitting the
backend that has not been checked.
