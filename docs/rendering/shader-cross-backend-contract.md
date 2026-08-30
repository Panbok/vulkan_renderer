---
status: partial
updated: 2026-08-30
authority: design
---

# Shader cross-backend contract

**Reviewed parity state: UNALIGNED.** The ledger accounts for every source under
`shaders/shared/`, `shaders/metal/`, and `shaders/vulkan/` as of 2026-08-30.
Normal decode, exposure, bloom, GTAO, native ABI reflection, and the
indirect-diffuse capture channel are **ALIGNED**. The broader rows remain
**UNALIGNED** where matched payload comparisons or focused fixtures are absent,
or where current native values disagree. The document remains `partial` until
those gates close.

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
| Transmission surface, volume, roughness, and production specialization | `shaders/shared/transmission_kernel.slangh` | `metal/msl/world/gpu_draws.metal`; paired diagnostic, production-temporal, and production-nontemporal entry points for compact and fullscreen launches; production `T == 1` pixels use specular-only direct/punctual/environment helpers; transmission ICB commands inherit parent-bound draw and peel roots | `vulkan/slang/world/deferred.slang`; the same paired entry-point family and resolved-pixel lobe selection | **UNALIGNED**: sources and host mirrors implement the same lobe partition, texture products, safe-`w` exit projection, Beer path, bounded rough LOD, compile-time specializations, and production diffuse omission at `T == 1`. Resolved material/ORM roughness zero samples ordered feedback; any positive value samples the immutable opaque pyramid at continuous IOR-adjusted LOD. The independent `0.04` BRDF floor does not alter feedback selection. Transmissive materials route only through the deferred peel stream, so both unreachable forward fallbacks are removed. Metal's missing peel-root inheritance is corrected, exact Bistro layers now diverge, CPU/reference and compiled ABI gates pass, and two corrected M1 Pro profiles pass the `0.400 ms` local ceiling. Native Vulkan startup reflection, Release execution, and synchronization validation now pass on the RX 6700 XT. Crossed HDR captures are still required |
| Packed geometry and GPU draw compaction | `shaders/shared/gpu_draw.slangh` for both Slang libraries | Native deferred/ICB path mirrors range-indexed decode records in `metal/msl/common/draw.metalh` and compacts in `metal/msl/world/gpu_draws.metal` | `vulkan/slang/common/resources.slangh`, `common/vertex.slangh`, and `world/deferred.slang` | **UNALIGNED**: v15 source and cooked paths publish one decode per range; candidate/visible rows retain their sizes and select the same range record in both source trees. Local shader compilation and reflection pass, but native Windows validation, a same-revision crossed payload comparison, and the deliberate-overflow fixture remain missing |
| Cascaded-shadow receiver | `shaders/shared/shadow_kernel.slangh` | `metal/msl/shadow/sampling.metalh`, used by forward and deferred paths | `vulkan/slang/world/default.slang`, reused by `world/deferred.slang` | **UNALIGNED**: sources agree and the five-case 1/9/16/32 plus 16-tap early-out-off matrix passes on both backends with a fixed native shadow map. Missing: crossed payload comparisons plus cascade-blend and distance-fade coverage |
| IBL baking and sampling | SH diffuse math below; other bake math is backend-local | `metal/msl/ibl/*.metal*`, `metal/msl/world/lighting.metalh` | `vulkan/slang/ibl/*.slang*`, `vulkan/slang/world/default.slang` | **UNALIGNED**: focused native validation passes on Metal and Vulkan, and current HDR render-path snapshots are non-degenerate on both. No matched direct bake-output snapshot pair covers equirect conversion, SH diffuse response, and every prefilter mip |
| L2 diffuse coefficients (ADR-038) | `shaders/shared/sh_l2_kernel.slangh`, included by the Vulkan Slang library and concatenated into the MSL library | `metal/msl/ibl/sh_projection.metal` `vkr_metal_packet_ibl_sh`; final `sh_coefficients` and slot fields in `metal/msl/common/draw.metalh`; evaluation in `metal/msl/world/gpu_draws.metal` | `vulkan/slang/ibl/default.slang` `ibl_sh`; exact texel loads use a lazily-published 2D-array alias; `VkrVulkanIblShRoot` plus final `sh_coefficients` and slot fields in `vulkan/slang/common/resources.slangh`; evaluation in `vulkan/slang/world/default.slang` | **UNALIGNED**: both production sources implement the same projection/evaluation contract; both 48-byte roots have compiled-shader reflection witnesses; focused native validation executes one packed probe on both backends; and the retained numeric Metal/Vulkan payload comparison passes. Missing: GPU repetition of the CPU projection fixtures |
| Indirect-diffuse capture channel (ADR-038 §3.1) | None | `metal/msl/world/gpu_draws.metal`, deferred lighting and transmission lighting | `vulkan/slang/world/deferred.slang`, `vk_deferred_lighting` and `vk_transmission_shade` | **ALIGNED**: both production sources implement render mode 9 with the same rule (environment diffuse only, black background, GTAO excluded, no direct/specular/emissive/ambient fallback). Three current Vulkan captures compare against retained Metal generation `sha256:7492b6406ad11123e0cb5f0f943f5c74bd908e3f72b13750c1a8fd1196f6e726` with MAE `5.080678104575146e-06`, maximum absolute delta `1/255`, and zero failed pixels; current report `sha256:330b26beef84ffd6f83c6e1182e6510ab9756c8307f99ec38c5fd246f5524e5b` |
| Visibility, deferred resolve, lighting, temporal resolve, transmission, and picking | Packed rows, range decode, and normal decode above | `metal/msl/world/gpu_draws.metal` | `vulkan/slang/world/deferred.slang`, `vulkan/slang/picking/default.slang` | **UNALIGNED**: raster and reconstruction use the compacted visible row's `decode_index` on both source paths, while the prior source algorithms and dispatch semantics remain aligned. Local ABI gates pass; native Windows validation and a crossed same-revision capture comparison remain required |
| Tonemap and FXAA | Exposure state above | `metal/msl/post/tonemap.metal` | `vulkan/slang/post/default.slang`, `post/tonemap.slangh` | **UNALIGNED**: source behavior agrees and all four authored on/off combinations pass on both backends from the same byte-identical HDR input. Missing: crossed final-color comparisons against the authored limits |
| Text color and picking | None | `metal/msl/text/default.metal` | `vulkan/slang/text/default.slang` | **UNALIGNED**: sources agree and the same fixture renders non-degenerate color and picking output on both backends. The native picking payload digests differ; a crossed color, coverage, and ID comparison is missing |

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
| `shaders/shared/sh_l2_kernel.slangh` | SH basis, packing, evaluation, and exact cubemap texel solid angle in section 4.7 |
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

The Metal tree has 19 production files and 48 distinct entry points: 45 native
MSL entry points and three Slang entry points compiled to Metal. `lib/CMakeLists.txt`
builds the native library by concatenating shared math first, then Metal headers
and implementation files. `metal/slang/library.slang` is compiled separately.

| Domain | Metal files and entry points | Vulkan counterpart | Review state |
| --- | --- | --- | --- |
| Packed geometry and draw vertices | `metal/slang/common/draw.slangh`, `metal/slang/world/default.slang`, and `metal/slang/picking/world.slang`; `vkr_metal_packet_vertex`, `vkr_metal_packet_temporal_vertex`, `vkr_metal_packet_picking_fragment` | `vulkan/slang/common/resources.slangh`, `common/vertex.slangh`, `world/default.slang`, and `picking/default.slang` | **UNALIGNED**: source counterparts and normalized native executions exist; a crossed same-case comparison and deliberate-overflow fixture remain missing |
| Native draw/resource declarations | `metal/msl/common/draw.metalh`; no entry point; this native MSL path mirrors rather than includes `shaders/shared/gpu_draw.slangh` | Vulkan resource and vertex headers above include the shared Slang records | **ALIGNED ABI**: native declarations remain independent, but Metal reflection and Vulkan SPIR-V reflection validate their semantic records and roots before use |
| Fullscreen triangle | `metal/msl/common/fullscreen.metalh`; helper only | `vulkan/slang/common/fullscreen.slang` | **UNALIGNED**: source counterpart and bilateral runtime witnesses exist; the newly reviewed final-color payloads have not been compared |
| Lighting and shadow sampling | `metal/msl/world/lighting.metalh`, `metal/msl/shadow/sampling.metalh`; shared progressive PCF comes from `shaders/shared/shadow_kernel.slangh` | `vulkan/slang/world/default.slang` and the same shared shadow kernel | **UNALIGNED**: source counterpart and bilateral five-case native snapshots exist; crossed receiver payloads, cascade blend, and distance fade remain missing |
| GPU draw generation, visibility, resolve, deferred lighting, temporal history, transmission, picking, HZB, and SDSM | `metal/msl/world/gpu_draws.metal`; 23 entry points from `vkr_metal_packet_vbuffer_fragment` through `vkr_metal_packet_sdsm_reduce` | `vulkan/slang/world/deferred.slang` | **UNALIGNED**: source and dispatch semantics plus focused native validation pass; the transmission family has paired diagnostic, production-temporal, and production-nontemporal variants for both launch shapes. Normalized Release execution is bilateral, but transmission layer counts disagree and capture comparison remains missing |
| Forward world shading and temporal MRT output | `metal/msl/world/default.metal`; `vkr_metal_packet_opaque_fragment`, `vkr_metal_packet_temporal_blend_fragment` | `vulkan/slang/world/default.slang` | **UNALIGNED**: source counterparts and normalized native executions exist; crossed output comparison remains missing |
| IBL baking | `metal/msl/ibl/common.metalh` plus equirect, SH projection, and prefilter `.metal` entry points | `vulkan/slang/ibl/default.slang` | **UNALIGNED**: source counterparts and focused native validation pass on both backends; current HDR render-path snapshots pass, but direct matched bake snapshots are missing |
| Text and text picking | `metal/msl/text/default.metal`; vertex, color-fragment, and picking-fragment entry points | `vulkan/slang/text/default.slang` | **UNALIGNED**: source counterparts and bilateral text snapshots exist; native picking payload digests differ and no crossed comparison has evaluated them |
| Tonemap and FXAA | `metal/msl/post/tonemap.metal`; fullscreen vertex and tonemap fragment | `vulkan/slang/post/default.slang` and `post/tonemap.slangh` | **UNALIGNED**: enabled and disabled source behavior agrees, and the four-state matrix passes bilaterally from one identical HDR input; crossed final-color comparisons remain missing |
| Exposure, bloom, and GTAO | the three files listed in the original topology table; 12 compute entry points | the three Vulkan post-effect files listed above | **ALIGNED** |

The broad `gpu_draws.metal` row contains three visibility fragments and twenty
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
| GPU classify and encode | two-dimensional grid `candidate_count x view_count`; `64x1x1` threadgroups | two-dimensional grid `candidate_count x view_count`; `64x1x1` shader groups | source-aligned; normalized native runtimes pass; matched overflow fixture remains |
| GPU prefix | one grid thread per view; one threadgroup whose width is `view_count` | one `5x1x1` group for the normal five-view contract; a second group preserves Vulkan's existing nine-view capacity | source-aligned; normalized native runtimes pass; deliberate-overflow fixture remains |
| Temporal transform | one thread per instance; `64x1x1` | one thread per instance; `64x1x1` | source-aligned; normalized native runtimes pass; crossed payload comparison remains |
| G-buffer, deferred lighting, temporal resolve, HZB, transmission shade/coverage, bloom, and GTAO | extent grid; `8x8x1` | extent grid; `8x8x1` | source-aligned where both entry points exist; normalized native runtimes pass; transmission coverage values disagree |
| Transmission compaction | extent grid at `8x8x1`, then one `1x1x1` finalize dispatch that writes indirect threadgroups for a `64x1x1` compact shade dispatch | the same scan/finalize/indirect shape with Vulkan-native storage descriptors and barriers | source/dispatch aligned; both focused validation gates pass, but current per-layer coverage counts disagree |
| Picking resolve | one `1x1x1` dispatch | one `1x1x1` dispatch | source-aligned; normalized native runtimes pass; current native payload digests differ and need crossed comparison |
| SDSM reduction | extent grid; `16x16x1` | extent grid; `16x16x1` | source-aligned; matched runtime gate missing |
| IBL bake | cube extent by six faces; `8x8x1` | cube extent by six faces; `8x8x1` | source-aligned; runtime gate missing |

The historical `p20_metal_state_matrix` and `p20_vulkan_state_matrix` cases
cannot close these rows because their authored states differ. The backend-neutral
`smoke.deferred.state_matrix.snapshot` case now supplies identical scene state,
capture frames, channels, and assertions. Both backends and both focused native
validation gates pass. Closure still requires a crossed payload comparison, a
deliberate-overflow fixture, and resolution of the transmission-layer count
disagreement recorded in sections 4.8 and 5.4.

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

### 3.3 Packed rows and backend-native roots — ALIGNED ABI

The shared packed records have these host contracts:

| Record | Size | Alignment | Important offsets |
| --- | ---: | ---: | --- |
| `VkrPackedStaticVertex` | 32 | 4 | `words[8]` at `0` |
| `VkrGpuGeometryDecodeRecord` | 32 | 4 | position bias `0`, flags `12`, position scale `16` |
| `VkrGpuGeometryRow` | 48 | 8 | vertex/index addresses `0/8`, first vertex/index `16/20`, layout `28`, generation `32`, decode address `40` |
| `VkrGpuCandidateDrawRow` | 48 | 16 | geometry/material/instance `0/4/8`, index range `12/16`, vertex offset `20`, decode index/state flags `24/28`, sphere `32` |
| `VkrGpuVisibleDrawRow` | 32 | 4 | geometry through state flags at offsets `0..28`, including decode index `24` |

`vkr_gpu_abi.c` validates every listed host offset. Vulkan pipeline creation
reflects packed vertices, geometry, candidate and visible rows, the 144-byte
material row, packet draw/frame roots, the 48-byte SH projection root, both
transmission roots, and the complete cull, G-buffer resolve, deferred-lighting,
temporal-resolve, picking, HZB, and SDSM roots. Metal reflects its native roots
and the nested records they reach. The native deferred library still declares
the packed records independently of `gpu_draw.slangh`, so reflection is the
required drift guard.

All Vulkan shaders share a 16-byte push constant containing the root device
address at `0`, material index at `8`, and flags at `12`. Sampled texture aliases
share set 0 binding 0, sampler and comparison-sampler aliases share set 0
binding 1, and storage-image aliases share set 1 binding 0. The host pins the
following Vulkan root sizes; the packet, GTAO, SH projection, and transmission
roots have the field-level SPIR-V reflection described above:

| Root | Bytes | Root | Bytes |
| --- | ---: | --- | ---: |
| cull | 176 | raster | 48 |
| temporal transform | 32 | G-buffer resolve | 352 |
| temporal resolve | 128 | deferred lighting | 128 |
| HZB | 48 | SDSM | 32 |
| picking | 64 | transmission shade | 448 |
| transmission compact | 80 | transmission coverage | 32 |
| IBL bake | 32 | IBL SH projection | 48 |
| packet frame | 480 | packet draw | 48 |
| packet utility | 544 | — | — |

The ABI portion is **ALIGNED**: the independent native MSL declaration is
guarded by field-level reflection, and Vulkan now checks the candidate record
and every listed deferred compute root from compiled SPIR-V. The ABI gate is
closed. The broader packed-geometry row remains **UNALIGNED** for the deliberate-
overflow fixture, crossed Release comparison, and transmission-layer finding
described in sections 4.5, 4.8, and 5.4.

### 3.4 Complete Metal reflection manifest

The Metal manifest has 34 reflected records. This table records their native
byte sizes; `metal/vkr_metal_packet_abi.c` remains the field-name and offset
authority. Pipeline creation compares every manifest field offset, record size,
and required alignment with native Metal reflection. It also checks nested
records reached through draw and frame pointers.

| Group | Reflected Metal records and byte sizes |
| --- | --- |
| Geometry and scene rows | vertex `64`; instance `80`; material base `176`; transmission extension `32`; text vertex `32`; IBL probe `64`; shadow cascade `96` |
| Draw roots | vertex draw `48`; temporal vertex draw `48`; draw `48`; frame `464` |
| Presentation and bake roots | tonemap `32`; equirect `32`; SH projection `48`; prefilter `32`; text `176` |
| Visibility roots | GPU draw `192`; transmission peel `16`; temporal transform `32`; G-buffer resolve `352` |
| Shared post-effect records | GTAO params `192`; GTAO depth `224`; GTAO evaluate `256`; GTAO denoise `240`; exposure `112`; bloom `80` |
| Lighting, temporal, and transmission roots | deferred lighting `160`; temporal resolve `208`; transmission shade `464`; transmission coverage `32`; transmission compact `96` |
| Utility roots | picking resolve `128`; HZB build `48`; SDSM `32` |

The host gives root uploads 256-byte alignment and reserves 512 bytes for each
draw-root ring cell. Other shader-visible size assertions cover the 32-byte
packed static vertex, 32-byte geometry decode record, 80-byte GPU draw
compaction state, 116-byte transmission diagnostics record, and 16-byte SDSM
state. The frame root keeps the retired 64-bit BRDF-LUT slot as named padding at
offset 104 so later fields do not move.

| Metal constant | Value |
| --- | ---: |
| maximum GPU draw views | 5 |
| transmission visibility layers | 4 |
| capture-only diagnostic transmission layers | 5 |
| temporal transform capacity | 32,768 |
| root upload alignment | 256 bytes |
| draw-root ring stride | 512 bytes |

Different native sizes are expected where the resource representation differs.
For example, Metal/Vulkan material bases are `176/144` bytes and their same-slot
transmission extensions are `32/16` bytes, frame roots are
`464/480`, deferred-lighting roots are `160/128`, temporal-resolve roots are
`208/128`, transmission-shade roots are `464/464` after both gained the
compact-list addresses and controls, and picking-resolve roots are `128/64`.
These size differences do not establish an algorithm mismatch;
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

GPU draw classification and encoding use `64x1` groups and a two-dimensional
`candidate_count x view_count` grid. Metal's normal contract is at most five
views. Vulkan retains its existing camera-plus-eight-cascade capacity, so its
prefix runs one `5x1` group for the normal contract and a second only above five
views; inactive lanes return. The cold packet boundary limits candidates and
visible rows to 262,144. The state buckets are opaque back-face culling, opaque
double-sided, cutout back-face culling, and cutout double-sided.

Metal's encode kernel compacts visible rows and writes indexed-triangle commands
into ICBs. Each command binds the per-view draw root at vertex buffer 0 and
fragment buffer 1.

Both backends assign each bucket a 65,536-command quarter capacity. Prefix
clamps each bucket independently, preserves every other bucket, and counts only
the dropped rows. Vulkan command storage is physically fixed-partitioned;
visible rows and Metal ICB commands remain compact in bucket order. Closing the
remaining evidence gap requires a fixture that covers both a single bucket
above 65,536 with total work below 262,144 and total work above 262,144.
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

The shared table and both receiver sources agree. The bounded
`shadow_receiver` scene contains visible lit, shadowed, and penumbra regions.
Five Release snapshots on each backend cover 1, 9, 16, and 32 taps plus 16 taps
with the uniform-region early out disabled. Each native shadow map stays fixed
while the shadow-factor and final-color digests change for every authored
state. Metal's cascade-0 digest is
`sha256:d239441aabfe2b5cc30e46e3194a30d6694820660bb57b4b45edffa9b3f8a398`;
Vulkan's is
`sha256:1430b7f64030052bfd14aa85d49f81e60e0e5adff0595e6c0fe5414b0752c37a`.
The state remains **UNALIGNED** until crossed comparisons evaluate the receiver
payloads. Cascade blend and distance fade also remain outside this focused
matrix.

### 4.7 IBL bake and sampling — UNALIGNED

Equirectangular conversion and specular prefilter dispatch `8x8x6` and use the
same cube-face orientation and longitude/latitude mapping. Specular prefilter
uses 256 Hammersley GGX samples, PDF-derived sample solid angle, cube texel solid
angle, and mip LOD; roughness at or below `0.001` forces mip 0. Diffuse response
instead dispatches one 64-thread workgroup that projects the selected cube mip
into nine normalized L2 coefficients, packs seven `float4` values, and publishes
a completion-safe slot. Forward and deferred lighting use the same analytic
environment BRDF, local-probe box weighting, and global-probe remainder; box
projection remains specular-only.

The SH projection root is 48 bytes on both host contracts. Metal compiled-root
reflection and focused API/GPU shader validation pass. Vulkan now compiles the
SH entry with a typed projection-root pointer, reflects every field and the
48-byte extent, and passes two native API plus synchronization-validation
repetitions. Exact projection reads use a lazily-published `Texture2DArray` view
of the source cubemap; the filtered cube view remains unchanged for skybox and
specular work. Current Release HDR render-path snapshots are non-degenerate on
both native backends. The Metal report is
`sha256:4cf4a6aa239b1e33507927aa43e837edb5ca2db86cd665d5eb0bc21f0dac618f`;
the Vulkan report is
`sha256:63988db941507a42b62ac1b8fed6ec307a10de9ff34af6ea9a3afafef7d9a089`.
They exercise the sampled result, not the direct bake products. The state
remains **UNALIGNED** until matched Release captures cover equirect conversion,
SH diffuse response, and every prefilter mip.

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
`1x1`; SDSM reduction is `16x16` on both backends. HZB stores the maximum of
each clamped 2x2 footprint and Metal includes
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

Both backends rasterize four transmission visibility layers. Their compact
paths scan each layer at `8x8`, use subgroup and threadgroup prefix sums to
append covered pixels, then a one-thread finalize kernel writes indirect
`64x1` shade dispatch arguments. The scan copies the layer background in the
same traversal, reserves the viewport-sized list once per threadgroup, bounds
every write, and records per-layer overflow. Metal uses native resource
references and encoder barriers; Vulkan uses bindless storage indices,
buffer-device addresses, synchronization2 graph barriers, and
`vkCmdDispatchIndirect`. The source contract is aligned. The normalized Release
case exercises all four coverage layers, compact scan/finalize, indirect
shading, and picking on both backends. Metal report
`sha256:c370fa5509cbbed16ba700f21fb33cdd1ff5c5e499b5fe9ffe243191e83b86eb`
and Vulkan report
`sha256:6d0205d3a354848b8a5a17a3fbe5a12ef7f9d132d457ad5a90028f10b3c3fc1d`
agree on opaque buckets `2/1/1/1`, transmission buckets `1/5/1/1`, and zero
draw, compact, resolve, publication, and shadow overflow. Their historical
coverage minima differed: Metal repeated `17970` in all four layers while
Vulkan reported `17970/10706/3040/2895`. The Metal transmission ICB peel-root
correction resolves that source defect; the current Metal case reports
`17970/10711/3040/2895`. Both focused native validation runs pass without a
validation or synchronization diagnostic.

The state remains **UNALIGNED** until a same-revision native Vulkan rerun and
one crossed Release comparison span visibility, G-buffer, lighting, temporal
history, transmission, and picking. The compiled-root and bilateral native
execution gates are closed.

### 4.9 Fullscreen output, ACES, and FXAA — UNALIGNED

Both backends use a three-vertex fullscreen triangle. The ACES fit constants
are `(2.51, 0.03, 2.43, 0.59, 0.14)`. FXAA uses square-root Rec. 709 luminance,
edge threshold `max(0.0312, luma_max * 0.125)`, direction reduction factor
`0.03125` with floor `0.0078125`, maximum span 8 pixels, and subpixel strength
`0.75`. In the Metal root, `reserved.x` enables ACES and `reserved.y` enables
FXAA. Those enabled paths agree with Vulkan's fullscreen flags.

The tonemap-disabled path matches Metal on both backends: multiply by exposure,
clamp negative values, then clamp the result to `[0, 1]`. Four backend-neutral
cases cross tonemap on/off with FXAA on/off, and all four pass on both backends.
Every run starts from the byte-identical HDR input
`sha256:7a691b08cc91b3bd4e8b87e87ed37b2af05b87f55bc15eebfea61fc680a16116`
and produces four distinct native final outputs. This contract remains
**UNALIGNED** until crossed final-color comparisons evaluate the authored
maximum-delta, mean-error, and failed-pixel limits.

### 4.10 Text color and picking — UNALIGNED

Bitmap text uses atlas alpha. MTSDF text uses the median of RGB minus `0.5`,
then derivative width `max(fwidth(distance), 1e-6)` and a saturated `+0.5`
coverage reconstruction. Color output preserves glyph RGB and multiplies glyph
alpha by coverage. Metal selects MTSDF when `controls.y > 0.5`; UI flag bit 0
maps through the pixel extent in `controls.zw`. Picking discards coverage at or
below `0.01` and otherwise returns the draw object ID. Screen-space clip
conversion differs in Y between the native APIs to produce the same top-left UI
convention.

The audited sources agree. The same Release fixture visibly exercises system,
bitmap, UI MTSDF, and world MTSDF text plus picking on both backends. Metal
report `sha256:ae31212817546ad4a47e3753174a283bc96b1b7a7f641ded5b977194e15801ce`
produces final-color digest
`sha256:7ce12e1724df9037b6735ff61cbcb9216e1afaf9e781a9733ecf28509fb91b62`
and picking digest
`sha256:6d646162b03d58abc081b59e02c2b60bcc2883e9e6b6636ba37a325d7aafa186`.
Vulkan report
`sha256:1a7de51d4d077808869c8689c86700661e04f7503087d34654a88b6b5a1949e7`
produces final-color digest
`sha256:9566e77427690003479c810cefa7c87b7077049d57768368c83a9808f45ed237`
and picking digest
`sha256:ebc8eb20d2027f9145306c2a6e0621dec642f5bd93d02efc725e06ef4262472b`.
The state remains **UNALIGNED** until crossed comparisons evaluate color,
coverage, and object IDs with documented limits.

## 5. Cross-backend runtime witnesses

All rows are Release snapshots with validation layers disabled. These are
correctness witnesses, not performance comparisons or accepted golden images.

| Contract | Metal witness | Vulkan witness | Recorded real values |
| --- | --- | --- | --- |
| Normal decode, exposure, GTAO on | M1 Pro, `mac_bistro_exposure_parity`, `sha256:66737ad0e780cb142756fb687f7b1e4a5a1d0f3683b156dcbc246b5de2f748e2` | RX 6700 XT, matched Bistro case, `sha256:dd4dd35397fb2b83d73e571c43d44a7a3431068c4a2fd2695d2e7b31e2c23a72` | average log luminance `-3.309451` / `-3.308282`; EV `+0.835520` / `+0.834351`; multiplier `1.784500` / `1.783055` |
| Exposure, GTAO off | M1 Pro, `mac_bistro_exposure_parity_gtao_off`, `sha256:87ef98262d33a9e6c9db87a78a779c4579ece6fd2e7402003a545479ccfdad50` | RX 6700 XT, matched Bistro case, `sha256:3e497612b394ead71cbb876c669cec7db4ae44230db8db0eab3a5ce50ce235b1` | average log luminance `-3.118539` / `-3.115359`; EV `+0.644608` / `+0.641427`; multiplier `1.563315` / `1.559872` |
| Bloom execution | M1 Pro, `smoke.bloom.static`, `sha256:333b97da86b2e226c7a9528ff579888e80c70cb2034d4655a51acaea07151659` | RX 6700 XT, `smoke.bloom.static`, `sha256:3af307a224ca770abe8a9efd2e81b8d6e1411a4acdbcf780299f606604a4c260` | Metal's prefilter/result are non-degenerate; Vulkan's prefilter, result, combined HDR, and final color have distinct digests |
| L2 indirect diffuse | M1 Pro / Metal 4, `smoke.sh_ibl.single_probe.snapshot`, `sha256:6e8431ecff0626fedb99104236941f0f4546fa0fef7a6ab1cf07c47a0eaa6451`; portable generation `sha256:7492b6406ad11123e0cb5f0f943f5c74bd908e3f72b13750c1a8fd1196f6e726` | RX 6700 XT / Vulkan 1.4, same case; cross-backend report `sha256:549304f5a5d769943853ef46d70a281795c773ca358ff35d6bf69866790c7f32` | Each backend packs one probe and produces three internally byte-identical `640x480` RGBA8 captures. Metal data is `sha256:183eed0d3d791e410cbfa00876aa80556aecf4135480dc538f2f705e5c0c78b1`; Vulkan data is `sha256:a5b95003b45846451508acb29b481e6bd013945daa69b6075c8737b7aac1c64a`. All three 1,228,800-value comparisons report MAE `5.080678104575146e-06`, maximum absolute delta `0.00392156862745098` (`1/255`), zero failed values/pixels, and failed-pixel ratio `0`. Limits are `2/255`, `0.1/255`, and `0.001` respectively |

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

The post-alignment Release snapshot on 2026-08-29 passed as report
`sha256:c4e8011f6ac91a6ef584acd988fcc28a3093dab9269b91b0eaec84021d4076a6`.
All six serial replay children passed. A representative 30-frame child passed
15/15 assertions with opaque buckets `2/1/1/1`, transmission buckets
`1/1/1/1`, zero opaque/transmission/compact/shadow overflow, zero invalid
G-buffer resolves, and valid HZB history for 30/30 samples. All four
transmission layers reported nonzero coverage; layer 0 ranged from 15,046 to
15,378 pixels. The report is intentionally non-authoritative because the
profile is local-only, the implementation tree was dirty, and no accepted
baseline exists. It proves the changed Metal production path remains
non-degenerate; it is not a performance comparison or Vulkan evidence.

The focused post-alignment Debug validation replay disabled optional HZB
occlusion with `VKR_HZB_DISABLED=1` and enabled both Metal API and GPU shader
validation. Report
`sha256:b7fb6d268323d995c13015d7d6a81c1497b019b0c2a0dc03b21ae7b0b0bd94cd`
passed all 11 assertions after a 30-frame warmup: opaque buckets were
`2/1/1/1`, transmission buckets were `1/1/1/1`, and draw overflow and invalid
G-buffer resolves were zero. The one child was the only renderer process, and
stderr contained only the two validator-enablement notices. A separate
production-HZB diagnostic had clean validator output but rejected every state-
matrix candidate after completed history became available, so its parent
visibility assertions failed; it is not counted as a pass. The ordinary
Release snapshot above remains the production HZB-on witness with 30/30 valid
history samples. Neither diagnostic is performance evidence.

The changed fixed-count IBL convolution also executed in the Release
`smoke.hdr_environment.snapshot` case. Report
`sha256:e91c18b5937c2eeb7fdf8ab89452aec24a49d8be9780d44852f1f296f0d400bf`
captured distinct non-degenerate 640x480 final and HDR scene-color payloads
(`sha256:9fb12f589307710a27b6733105d0140fb4e748dcd4144b7a4ffab7c95d939c5c`
and
`sha256:008e9171603e352a51dd3b72a58cf91008f9aca9af9e42f4455d254f1c70f14c`)
with 31/31 opaque draws visible and zero draw overflow. Visual inspection found
textured lit geometry rather than a clear or error frame. With a dirty local
tree and no accepted baseline, this is Metal execution evidence only.

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

### 5.3 Current Windows closure pass

The 2026-08-29/30 Windows pass used the Release Vulkan binary on an AMD Radeon
RX 6700 XT with driver 26.6.3 and validation disabled. The separate Debug gates
enabled `VK_LAYER_KHRONOS_validation` and synchronization validation. All runs
used the dirty implementation tree at source revision
`f6b0314fb30a1840fad7539396f0071f95247d15`; they are local correctness
witnesses, not accepted baselines or performance evidence.

| Contract | Current Windows witness | Recorded result |
| --- | --- | --- |
| Deferred, packed rows, and four-layer compact transmission | `smoke.deferred.state_matrix.snapshot`, report `sha256:6d0205d3a354848b8a5a17a3fbe5a12ef7f9d132d457ad5a90028f10b3c3fc1d` | Opaque buckets `2/1/1/1`; transmission buckets `1/5/1/1`; coverage-layer minima `17970/10706/3040/2895`; draw, compact, resolve, publication, and four shadow-overflow counters all zero |
| Deferred synchronization validation | `local.deferred.state_matrix_vulkan_validation`, report `sha256:f9c62b4ccee4563e85e345f02f5052c70698f4f21da4b55e189c9af80d8e93e8` | Two serial repetitions and all 17 assertions pass; no VUID, validation error, or synchronization hazard occurs |
| Cascaded-shadow receiver | Five `smoke.shadow.receiver.*` reports: `sha256:88cc458ee7c543bde0e57cf083ebb2e05a722a2dfa782f59b6b9b48811ba57c1`, `sha256:a8c01cbec50799a857028695888b1cc218cbaff90722a373ca268d1a02306052`, `sha256:9358e00397d768010dfdc2e0de18c44e98eb80f2db94c535f3e98cc90304ccc0`, `sha256:7e0214c655f20118215baa71408f6c89f8795c3e2a386f86f9233af1f1a2baf7`, and `sha256:96a9c249e29c3e9087c9150e989f282a668dd0f77f7413aca06c2af9f4709db3` | PCF 1/9/16/32 and 16-tap early-out-off all pass. Cascade 0 stays `sha256:1430b7f64030052bfd14aa85d49f81e60e0e5adff0595e6c0fe5414b0752c37a`; every shadow-factor and final-color digest differs |
| Tonemap/FXAA authored matrix | Four current `smoke.post.tonemap_fxaa.*` reports: `sha256:acfc916d2fe630030a5fb7a7a097827d7d786bda5a6aaa671e210357f535adda`, `sha256:9d1d9141dcaf8eac05185647bdadf9add247d583d67703f3df7b08ecab446e6d`, `sha256:53b35f04d5724bfb0ebaf376b2850f3c3399c92b3b1141f14e817cdd7ddf42b1`, and `sha256:a8817f6d5593dfbca0f0488988642c433d6f406e3c4b46b65b5e4db1beafaa52` | One fixed HDR digest `sha256:7a691b08cc91b3bd4e8b87e87ed37b2af05b87f55bc15eebfea61fc680a16116`; four distinct workload fingerprints and final outputs |
| Text color and picking | `smoke.text.rendering.snapshot`, report `sha256:1a7de51d4d077808869c8689c86700661e04f7503087d34654a88b6b5a1949e7` | System, bitmap, UI MTSDF, and world MTSDF text are visible; final color is `sha256:9566e77427690003479c810cefa7c87b7077049d57768368c83a9808f45ed237`; picking is `sha256:ebc8eb20d2027f9145306c2a6e0621dec642f5bd93d02efc725e06ef4262472b` |
| HDR/IBL execution | `smoke.hdr.environment.snapshot`, report `sha256:63988db941507a42b62ac1b8fed6ec307a10de9ff34af6ea9a3afafef7d9a089` | Pass; non-degenerate final, scene-linear HDR, and depth captures are produced; scene color is `sha256:6c5cc35cb016b4e889a8907c81ab3ae2b086728e4a07ec15b867113b28e2b8e6` |
| SH/IBL focused validation | `local.sh_ibl.single_probe_vulkan_validation`, report `sha256:e18f00b4b3602409e5707555ec0b0ca4a5ec5b5d8683be53e296f31ad791320e` | Two serial repetitions pass, one probe is packed in every measured frame, and validation logs contain no VUID or synchronization hazard |
| Indirect-diffuse cross-backend comparison | Current `smoke.sh_ibl.single_probe.snapshot`, report `sha256:330b26beef84ffd6f83c6e1182e6510ab9756c8307f99ec38c5fd246f5524e5b` against retained Metal generation `sha256:7492b6406ad11123e0cb5f0f943f5c74bd908e3f72b13750c1a8fd1196f6e726` | All three comparisons pass with workload fingerprint `sha256:5ba3455d443ec125efdda609be5febbe750dbdfde49a852c80409fedad69b0dc`, MAE `5.080678104575146e-06`, maximum delta `1/255`, and zero failed values or pixels |

The four post cases, five shadow cases, normalized deferred case, and text case
are backend-neutral. No accepted baseline pointer changed in this Windows pass.

### 5.4 Current Metal closure pass

The 2026-08-30 Metal pass used the clean Release binary at
`2f42e21e4240f4cc4e23c4eb450f5de8b8a64082` on an Apple M1 Pro / Apple Metal 4
GPU with both validation variables unset. The separate Debug gate enabled Metal
API and GPU shader validation in exactly one renderer process.

| Contract | Current Metal witness | Recorded result |
| --- | --- | --- |
| Deferred, packed rows, and four-layer compact transmission | `smoke.deferred.state_matrix.snapshot`, report `sha256:c370fa5509cbbed16ba700f21fb33cdd1ff5c5e499b5fe9ffe243191e83b86eb` | Nine capture children pass. Opaque buckets are `2/1/1/1`; transmission buckets are `1/5/1/1`; draw, compact, resolve, publication, and shadow-overflow counts are zero. Coverage-layer minima are `17970/17970/17970/17970`, which disagrees with Vulkan's `17970/10706/3040/2895` |
| Focused Metal validation | `local.p20.metal.state_matrix.validation`, report `sha256:f4255516d35516a66719787c7a7c09d930d09edc39c7c430669d80745984edc2` | All 11 assertions pass with both validators enabled; no validation error, hazard, or fatal diagnostic occurs. The local diagnostic reports warmup instability and supplies no performance evidence |
| Cascaded-shadow receiver | Five `smoke.shadow.receiver.*` reports: `sha256:a49e9f56ac7eb6633b46d1916e4369841035ec6386c5985b33595f6199d94bf7`, `sha256:64256412e311c339f831fb4cc481cbb4f23ff21c8c600e3ce4cbc867722cc1a8`, `sha256:cb7813baa459f13fb6ac3903dfe282d20e23c2273c3910defbda2f955f133f7a`, `sha256:697e9046c421b6e3a711a2833a92b317ab3d825e28418271006d2c9a9b635807`, and `sha256:a5d1b26213bd04c2b137e3f2647b383bea2c56447c2a262e9b7931c985e0db0c` | PCF 1/9/16/32 and 16-tap early-out-off all pass. Cascade 0 stays `sha256:d239441aabfe2b5cc30e46e3194a30d6694820660bb57b4b45edffa9b3f8a398`; every shadow-factor and final-color digest differs |
| Tonemap/FXAA authored matrix | Four `smoke.post.tonemap_fxaa.*` reports: `sha256:e891075a3e8c982e81f8eb51541bbf4b35961f3e75ddd526af800832f16637d3`, `sha256:361d6f2f1c3bc67b28beccb6bc4eed81d4a34d41fb7c91e7a48e34d3af9b8f4e`, `sha256:8fc82d911b5ef7294d3890df2534b608960d1f4df622f1c6bc292593cfb61c2d`, and `sha256:fb4594ba66507b18a06cda1c1a1980c5b279975d0436eb4d5f54db51c0564c46` | All four pass from the exact Vulkan HDR digest `sha256:7a691b08cc91b3bd4e8b87e87ed37b2af05b87f55bc15eebfea61fc680a16116` and produce four distinct final outputs |
| Text color and picking | `smoke.text.rendering.snapshot`, report `sha256:ae31212817546ad4a47e3753174a283bc96b1b7a7f641ded5b977194e15801ce` | System, bitmap, UI MTSDF, and world MTSDF text are visible; final color is `sha256:7ce12e1724df9037b6735ff61cbcb9216e1afaf9e781a9733ecf28509fb91b62`; picking is `sha256:6d646162b03d58abc081b59e02c2b60bcc2883e9e6b6636ba37a325d7aafa186` |
| HDR/IBL execution | `smoke.hdr.environment.snapshot`, report `sha256:4cf4a6aa239b1e33507927aa43e837edb5ca2db86cd665d5eb0bc21f0dac618f` | Pass; non-degenerate final `sha256:dee0ce491e5137e131856deeb8ce1585590e675ab09a637be9fc08f0f273f5a6`, scene-linear HDR `sha256:3c9206136e78418096d3f71195fe100b4c0ed41156f0aa26d300a6061c659265`, and depth `sha256:bb0274040b0507a17189885edc60635b7766054b85b98acc983f958d12750c11` are produced |

These runs close the missing Metal execution and validation gates. They do not
replace crossed comparisons because the Windows commit retained report and data
digests, not the canonical payloads needed to evaluate the authored envelopes.
No accepted baseline pointer changed in this Metal pass.

The focused Vulkan SH witness uses
`tools/cases/local/sh_ibl_single_probe_vulkan_validation.case.json` with the
validation-windowed profile. The current integrated binary passes two
repetitions with report digest
`sha256:e18f00b4b3602409e5707555ec0b0ca4a5ec5b5d8683be53e296f31ad791320e`.
The backend-neutral snapshot case
`tools/cases/smoke/sh_ibl_single_probe_snapshot.case.json` produces three
byte-identical visible Vulkan captures with data digest
`sha256:a5b95003b45846451508acb29b481e6bd013945daa69b6075c8737b7aac1c64a`.
The same clean Release case on an Apple M1 Pro / Metal 4 GPU passes as report
`sha256:6e8431ecff0626fedb99104236941f0f4546fa0fef7a6ab1cf07c47a0eaa6451`.
It packs one probe and produces three byte-identical visible captures with data
digest
`sha256:183eed0d3d791e410cbfa00876aa80556aecf4135480dc538f2f705e5c0c78b1`.
The Metal image has 114,572 non-black pixels, 8 distinct RGBA values, and a
dominant foreground value of `(152, 157, 165, 255)` across 111,384 pixels.
The full Metal capture set is retained as accepted generation
`sha256:7492b6406ad11123e0cb5f0f943f5c74bd908e3f72b13750c1a8fd1196f6e726`
under `tools/baselines/local.offscreen/smoke.sh_ibl.single_probe.snapshot/`.

The 2026-08-29 Windows run compared fresh Vulkan captures with that retained
Metal generation. The workload fingerprint
`sha256:5ba3455d443ec125efdda609be5febbe750dbdfde49a852c80409fedad69b0dc`
and policy fingerprint
`sha256:d2f7e38ba7fc40b940e5b7abe51700cf14b51397d33af40316cbe793ff842081`
match the Metal record. All three comparisons pass. Each compares 1,228,800
RGBA values across 307,200 pixels with MAE `5.080678104575146e-06`, maximum
absolute delta `0.00392156862745098` (`1/255`), zero failed values or pixels,
and failed-pixel ratio `0`. The authored limits are `2/255`, `0.1/255`, and
`0.001`. Report digest:
`sha256:549304f5a5d769943853ef46d70a281795c773ca358ff35d6bf69866790c7f32`.
The `indirect_diffuse` row is therefore **ALIGNED**. GPU repetition of the CPU
projection fixtures remains open for the coefficient row.

The reproducing command is:

```sh
build_release/tools/vkr_harness snapshot \
  --case tools/cases/smoke/sh_ibl_single_probe_snapshot.case.json \
  --profile tools/profiles/local-offscreen.json \
  --cross-backend
```

The comparison reads retained canonical captures, verifies their digests,
requires matching workload and policy fingerprints, and applies the case's
authored pixel limits. Tonemap and FXAA are default-on controls. Their default
state is normalized out of the workload fingerprint so manifests authored
before those controls existed retain semantic identity; either disabled state
is fingerprinted explicitly. The current integrated run reuses the retained
generation and passes as report
`sha256:330b26beef84ffd6f83c6e1182e6510ab9756c8307f99ec38c5fd246f5524e5b`.
No pointer was changed here.

The 2026-08-30 transmission-correctness slice adds the shared composition and
rough-LOD kernel, unchanged `176/144`-byte material bases plus same-slot
`32/16`-byte transmission extensions, native exit-point projection, and a
116-byte diagnostics record with four production coverage counters plus one
diagnostic counter. The compact shader creates thin factor-only and extended
ranges; one partitioned indirect entry maps them to compile-time variants, and
the thin variant reads neither the extension nor optional texture/volume work.
`./build_test.sh` passes both generated shader targets and their reflection
checks. The focused Metal process ran with `MTL_DEBUG_LAYER=1` and
`MTL_SHADER_VALIDATION=1`; seven candidates executed with zero draw, compact,
or resolve-invalid overflow, report
`sha256:1f8bdaa03463cb42d568e138114f506449d6675373bf76fef867e8fcee30fe40`.
The follow-up Windows pass found and corrected an eight-byte host/Slang
transmission-root mismatch before pipeline creation. The 464-byte root now
contains the same explicit 16-byte pre-matrix padding in both representations,
and startup reflection covers all shade, compact, material-extension, and
coverage roots. A Release RX 6700 XT analytic snapshot passes all five replays
with exact `65,372/65,045/0/0` layer coverage and zero diagnostics, report
`sha256:32eb77a2c0e53d151fafaa55ecaa0ad15e89cf1dda934db1d438f5c47e6549df`.
The focused Debug synchronization-validation profile passes two serial
repetitions and all 18 assertions without VUID, validation error, or hazard,
report
`sha256:393dee3d19cab3544df1d149ec37e12b329064786d7722bad625bd1704f64ee7`.
The transmission row remains **UNALIGNED** until a crossed HDR comparison meets
the authored limits.

### 5.5 Metal transmission peel correction

The owner-camera rerun found that the Metal transmission ICB was created with
buffer inheritance disabled while the previous-depth peel root was bound only
on the parent render encoder. Every indirect fragment therefore observed a
disabled peel and repeated the front layer. Transmission now has a dedicated
inherited-buffer ICB and compile-time encode entry point; the parent binds the
draw roots at vertex/fragment indices 0/1 and the peel root at fragment index 2.
Opaque and shadow ICBs retain explicit per-command buffers.

The exact Bistro camera report
`sha256:6f4be0bb416f885c3ea743a28ad62b8552477a6d1882d1cae9220ebad50703bb`
preserves detail through both panes and records coverage
`63,695/2,033/771/652/164` with distinct layer hashes and zero diagnostics. The
current normalized Metal state-matrix report
`sha256:edb26f39222a283b946716853dcb527cbf69602c4c036bf4dd10f9ded6dfce0a`
records layer minima `17,970/10,711/3,040/2,895`, removing the former repeated-
layer source discrepancy. A focused serial API/GPU validation run is clean at
`sha256:f347ae9e7ecf84b2915fac729959f4449cb7974c45c634eb97933545408a2210`.
Native Vulkan validation now passes on the corrected root. Same-revision
crossed captures remain required, so the row stays **UNALIGNED**.

### 5.6 Gates that keep the remaining rows unaligned

The focused native validation pair is complete. Metal ran alone as required:

```sh
VKR_HZB_DISABLED=1 MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  build_debug/tools/vkr_harness profile \
  --case tools/cases/local/p20_metal_state_matrix_validation.case.json \
  --profile tools/profiles/local-metal-dual-validation-serial.json
build_debug/tools/vkr_harness.exe profile \
  --case tools/cases/local/p20_vulkan_state_matrix_validation.case.json \
  --profile tools/profiles/validation-windowed.json
```

Each closure record must carry both report digests and the actual comparison
values. Source, compiled ABI, Release execution, and focused validation now pass
bilaterally for the normalized deferred case. That row cannot become
**ALIGNED** until the transmission-layer count difference is resolved, a
deliberate-overflow fixture passes on both backends, and crossed captures meet
their authored limits.

Shadow, post, and text also have bilateral native execution reports but still
need guarded payload publication and crossed comparisons. The first machine
must publish through `baseline propose` and owner-approved `baseline accept`;
the second runs the same case with `--cross-backend`. Do not accept or move a
baseline pointer merely to make a comparison pass. Direct IBL bake still needs
the capture support described in section 4.7. GPU repetition of the CPU SH
projection fixtures remains the specific coefficient gate.

## 6. Maintenance rule

Shader work must use `.codex/skills/vkr-shaders/SKILL.md`. A change to a listed
value, structure, binding, entry point, or algorithm updates this ledger in the
same change. New facts enter as **UNALIGNED** until both production backends and
their evidence gates pass. Never preserve an **ALIGNED** label by omitting the
backend that has not been checked.
