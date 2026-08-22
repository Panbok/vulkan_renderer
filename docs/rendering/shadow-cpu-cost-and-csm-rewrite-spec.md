---
status: partial
updated: 2026-08-22
authority: design
---

# Directional shadow cost reduction and CSM rewrite

P0 instrumentation and P1 contract repair ship in the current implementation.
The renderer publishes the named CPU scopes and row-byte gauges on Metal and
Vulkan, routes raster depth bias through the packet on both backends, retains
the future fit data, and applies cascade-fit hysteresis. P2 and later phases do
not ship. The available P0 runs rejected P2 at 35 and 254 candidates, but they
were dirty-tree observations rather than authoritative profiles and do not
represent an identified larger reported workload. Exact unchanged-default
capture comparison also remains open for P1.

Prerequisite reading:
[shadow-transmission-transparency-improvements.md](shadow-transmission-transparency-improvements.md)
(the previous nuri-informed shadow proposal, parts of which are now stale),
[deferred-visibility-buffer/SPEC.md](deferred-visibility-buffer/SPEC.md) (the
GPU-driven topology every shadow cascade runs inside), and
[`.codex/skills/vkr-performance/SKILL.md`](../../.codex/skills/vkr-performance/SKILL.md)
(what counts as evidence here).

## 1. Summary and authorization boundary

The source audit supports four facts.

1. `vkr_shadow_system_update()` performs bounded cascade math. It does not walk
   scene objects, allocate, or interact with the GPU.
2. The application rebuilds the world candidate stream every frame. Vulkan and
   Metal then hash, resolve, and pack that stream again.
3. Temporal HZB rejection requires an exact view-projection and world epoch,
   and the shader uses it only for camera view index zero.
4. The current P21 receiver samples one shadow texel and drops most quality data
   produced by `VkrShadowSystem`.

Those are code facts. They do not prove which work dominates the reported CPU
cost. Candidate extraction, graph construction, command recording, and cascade
raster remain hypotheses until Phase 0 measures the affected workload in
Release.

This document authorizes no performance implementation before Phase 0. Each
later phase is a separate vertical slice. A phase does not inherit authorization
from an earlier phase, and no comparison may change the scene, camera path,
backend, target-image count, or metrics policy from its baseline.

The implementation order is:

1. instrument and identify the limiter;
2. add fit hysteresis and repair packet/config truthfulness without changing
   quality presets;
3. if measured, add backend-neutral candidate residency;
4. add retained shadow history before pass omission or staggering;
5. treat moving-camera temporal occlusion as a two-phase topology change;
6. add SDSM through completed asynchronous feedback;
7. restore PCF and receiver quality under a separate GPU budget.

Retained graph history, two-phase visibility, and asynchronous SDSM feedback
constrain future renderer work. Proposed ADRs must be accepted before those
implementation slices start. Section 12 names the decisions and owned files.

### 1.1 Current receiver quality

`packet_directional_shadow()` in `world/default.slang` and
`vkr_metal_packet_directional_shadow_sample()` in `shadow/sampling.metalh` both
do a single nearest-filtered tap with one scalar bias. No PCF. No cascade
blending. No normal or slope bias. Meanwhile `VkrShadowConfig` carries
`pcf_radius`, `normal_bias`, `shadow_slope_bias`, `shadow_bias_texel_scale`,
`shadow_uv_margin_scale_per`, `shadow_uv_soft_margin_scale_per`,
`shadow_uv_kernel_margin_scale_per`, `cascade_blend_range`,
`shadow_distance_fade_range`, and per-cascade `world_units_per_texel` and
`light_space_origin`. A repository-wide search for consumers of those fields
outside `vkr_shadow_system.c` and its header returns nothing. They are computed
each frame, packed into `VkrShadowFrameData`, and dropped, because
`VkrShadowPassPayload` only carries `light_view_proj[]`, `split_depths[]`, and
an optional depth-bias override. The architecture spec's claim that "the PCF
hash grid converts the fitted light-view origin into the shader's reconstructed
right/up basis" describes the world renderer that ADR-028 P21 deleted.

The receiver bias in use is not the config value either. It is a single global
constant, `vkr_packet_shadow_bias = 0.0001f` in `vkr_packet_constants.c`, shared
by all four cascades and both backends. Section 2.5 itemizes the resulting
quality gap. The architecture-spec sentence about a shipped PCF hash grid is
stale after ADR-028 P21 and must be corrected in the first implementation PR.

## 2. What we have today

### 2.1 Ownership

| Concern | Owner |
|---|---|
| Cascade splits, light view, ortho fit, texel snapping | `vkr_shadow_system.c` |
| Packet payload assembly | `application_build_world_payload()` and the shadow block in `application_draw_frame()` |
| Cascade pass expansion | `assets/render_graphs/main.rendergraph.json`, `Shadow.Cascade.${i}` with `repeat.count_source = shadow_cascade_count` |
| Multi-view culling input | `vkr_vk_deferred_cull_root()`, view 0 is the camera, views 1..N are cascades |
| Cascade recording | `vkr_vk_record_deferred_raster()` with `shadow = true` |
| Receiver sampling | `packet_directional_shadow()`, `vkr_metal_packet_directional_shadow_sample()` |

The shadow image is one array texture, `shadow_map`, with four D32 layers at
2048 square by default. One physical instance is 64 MiB. The resource is
`PER_IMAGE`, so total allocation is `64 MiB * target_image_count`: 128 MiB at
two target images and 192 MiB at three. Each cascade pass writes one layer
through an exact one-layer view and clears it first.

### 2.2 Per-frame work, per candidate

At default settings, for every submesh in the scene, every frame:

| Stage | Work |
|---|---|
| `application_build_world_payload()` pass 1 | count submeshes |
| pass 2 | material lookup, alpha routing, transmission test, frustum test for blend only |
| pass 3 | material lookup again, build a 144-byte `VkrWorldDrawCandidate` in the scratch arena |
| `vkr_vk_candidate_epoch()` | FNV hash over eight fields including the 64-byte model matrix |
| `vkr_vk_pack_gpu_candidates()` | resolve geometry table, resolve material table, write a 48-byte `VkrGpuCandidateDrawRow` and an 80-byte `VkrInstanceDataGPU` |

That is 128 bytes per candidate into the frame-upload ring plus 144 bytes of
scratch, five traversals, and two random-access table lookups. The backend also
rebuilds the fixed-capacity geometry table and packs the transmission subset.
Candidate capacity is `VKR_GPU_DRAW_CANDIDATE_CAPACITY`, 262144. How often the
set is unchanged, and whether these traversals matter at the reported candidate
count, are Phase 0 measurements rather than assumptions.

### 2.3 Per-frame work, per cascade

The graph is rebuilt from the parsed JSON model and recompiled every frame in
`vkr_vulkan_renderer_submit_packet()`. The four `Shadow.Cascade.${i}` passes are
re-expanded and rescheduled, and roughly fifteen downstream `shadow_map` read
declarations re-enter dependency and barrier planning. Graph images are cached
by descriptor equality in `vkr_vk_realize_graph_images()`, so no image is
recreated. The architecture spec records a historical measurement of the whole
graph at 0.3 to 0.5 ms out of a 20 ms frame, so the shadow share of graph
rebuild is a fraction of that, not the headline.

Every cascade clears and re-rasterizes unconditionally. There is no concept of a
cascade whose contents did not change.

### 2.4 Occlusion culling has narrow history acceptance

There is no camera depth pre-pass and there does not need to be one.
`VBuffer.Opaque` already performs that role. It writes depth plus a `uint2`
visibility ID, `vk_visibility_opaque_fragment` is `[earlydepthstencil]` with no
discard so opaque buckets get early-Z, and all shading happens later in
`Lighting.Deferred` as compute over the resolved G-buffer. Shading overdraw is
already zero. Adding a depth-only pass in front would re-run every vertex
shader for nothing.

What is missing is the occlusion culling that the HZB was built for. Two gates
in `vkr_vk_deferred_cull_root()` reject the history:

```c
candidate->history_world_epoch != slot->gpu_world_epoch ||
MemCompare(&candidate->history_view_projection,
           &current_view_projection, sizeof(Mat4)) != 0
```

There is no reprojection. Any camera motion fails the `MemCompare`. And
`gpu_world_epoch` is the FNV hash over every camera-opaque candidate produced by
`vkr_vk_candidate_epoch()`, so one object moving invalidates the HZB for the
whole scene. In any orbiting or free-camera case, occlusion culling contributes
nothing on moving frames. A static camera with an unchanged candidate epoch can
still use it.

The test is also camera-only. In `vk_gpu_draw_classify`:

```slang
if (view_index == 0u && vkr_vk_candidate_occluded(root, candidate))
```

Cascade views get frustum rejection only.

This is directly falsifiable in Phase 0. The backend count is exported as
`visibility.hzb.rejected`, alongside `visibility.hzb.history_valid`. If history
validity is zero on moving frames, the matrix-gate analysis above is confirmed.

The pass ordering also matters for any cascade-side fix.
`Shadow.Cascade.${i}` runs before `VBuffer.Opaque` and `HZB.BuildBase`, so
cascades cannot borrow the camera HZB and would need their own history.

### 2.5 The quality gap, itemized

The original report describes Nuri's directional shadows as better at distance.
The table below is a source comparison, not a controlled image comparison.

| Knob | Ours, `VKR_SHADOW_CONFIG_HIGH` | nuri default | nuri `High` preset |
|---|---|---|---|
| Cascades | 4 | 4 | 4 |
| Map size | 2048 | 2048 | 4096 |
| Max distance | 200 | 150 | 150 |
| Split lambda | 0.80 | 0.75 | 0.25 |
| Receiver filter | 1 nearest tap | 9 Poisson taps, comparison sampler | 24 taps |
| Constant bias | one global 0.0001 NDC | 0.0005 | 0.0005 |
| Slope bias | none | 1.5 | 1.5 |
| Normal bias | none | 0.0 | 0.50 |
| Cascade blend | computed, dropped | 0.08 of span | 0.08 |
| Distance fade | computed, dropped | 0.0 | 0.10 of range |
| Light-space Z fit | frustum slice plus `z_extension_factor * radius`, per cascade {2,3,4,5} | measured caster depth bounds | same |
| SDSM | none | on, temporal blend 0.85 | on |

**Our shadow draw distance is already longer than nuri's.** 200 against 150.
Raising it cannot explain or repair the reported difference without spending
more texel density. The following mechanisms are quality hypotheses for exact
capture A/Bs, not a ranked root-cause result.

**Split lambda favors the near field.** Both engines compute
`lambda * logarithmic + (1 - lambda) * uniform`. Ours is 0.80, so the split set
is heavily logarithmic and front-loaded, which gives cascade 0 a lot of texels
and dumps everything else into a very wide cascade 3. nuri's High preset uses
0.25, nearly uniform. With near 0.1 and far 200, our cascade 3 spans roughly
[54, 200], about 146 units. At lambda 0.25 the same cascade spans roughly
[120, 200], about 80 units. That is close to double the far-cascade texel
density for a one-line config change. nuri deliberately lowers lambda as its
presets get better, 0.35, 0.30, 0.25, which is the opposite direction from our
default.

**One nearest tap has no graceful failure mode.** A single unfiltered
comparison against a far cascade texel that covers a large world footprint
produces staircase noise that crawls as the camera moves. nuri's taps go through
a hardware comparison sampler, so each tap is already a bilinear 2x2 PCF, and
9 or 24 of them land on a rotated Poisson disk. The far cascade degrades into
softness rather than into aliasing. Measure the cost and compare exact captures.

**Three hard cascade seams.** We compute `cascade_blend_range` and drop it, so
each split boundary is a discontinuity in filter footprint and bias. nuri
cross-fades over 8 percent of each cascade's span. Hard seams inside the
shadowed range read as "shadows fall apart with distance" even when the shadows
themselves are fine.

**A hard terminating edge.** We compute `shadow_distance_fade_range` and drop
it, so shadows stop dead at the last split instead of fading out.

**Bias inflates with the Z extension.** Receiver bias is applied in NDC depth,
so the world-space offset it produces is roughly `ndc_bias * ortho_depth_range`.
Our Z range is not fitted to anything; it is the frustum slice extended by
`z_extension_factor * radius` with per-cascade factors {2, 3, 4, 5}. The far
cascade's bounding radius is one to two orders larger than cascade 0's, so the
same global 0.0001 becomes a far larger world-space offset out there. Working
the default config through with near 0.1, far 200, and a 45 degree vertical
field of view, cascade 3's ortho depth range comes out around six times wider
than its slice actually needs, and its effective world-space bias lands roughly
two orders above cascade 0's. That arithmetic is an estimate from the config,
not a measurement. The expected capture signature is larger receiver/caster
separation in distant cascades.

nuri avoids this by fitting Z to real geometry.
`fitDirectionalShadowCascadeSliceWithCasterDepthBounds()` takes measured caster
light-space depth bounds, built once per light direction from the static caster
cache in `ensureStaticCasterLightDepthBounds()` and combined with per-frame
dynamic caster bounds. VKR has related CPU math but no production bounds input.
`vkr_shadow_fit_relevant_caster_z()` exists, but
`VkrShadowSceneBounds.use_scene_bounds` defaults to `false_v` and the default
bounds are a hardcoded placeholder of plus or minus 20 units. Nothing computes
a real scene caster AABB, because no aggregate scene bounds exist anywhere in
`VkrMeshManager`. Enabling `use_scene_bounds` would also change light anchoring
and XY fit, so section 6.6 adds a separate Z-only input.

Split lambda, map size, distance, and filtering interact. Do not change them as
a bundle. Lambda 0.25 is a useful far-density experiment, not a universal
upgrade, and a 4096 D32 array costs 256 MiB per target image before any history
copy. Section 11 defines the controlled quality sweep.

## 3. What nuri does, and what is actually portable

nuri builds shadow draws on the CPU. We build them on the GPU. Copying nuri's
structure would move work back onto the thread we are trying to unload, so the
port is by technique, not by code.

| nuri mechanism | Where it lives | Port? |
|---|---|---|
| Static and dynamic caster split, cached per scene version | `rebuildSceneCache()`, `staticShadowTemplateIndices_` against `dynamicShadowTemplateIndices_` | Yes, as a candidate-stream partition |
| Static caster cache keyed on transform, LOD, and pipeline signature | `rebuildStaticShadowCasterCache()` | Yes, as persistent GPU candidate residency |
| Static-only cascade reuse that skips the depth pass entirely | `StaticOnlyCascadeReuseState`, `appendShadowDepthPasses()` early `continue` | Conditional on retained-resource semantics and measurement |
| Guard-banded rendered fit plus predictive motion margin | `applyStaticOnlyGuardBand()`, `staticOnlyDirectedMotionTexels()` | Yes, it is what makes reuse survive camera motion |
| Fit hysteresis and deadbands against the previous frame's fit | `applyDirectionalShadowFitHysteresis()` in `shadow_math.h` | Yes, as CPU math with capture validation |
| Light-space Z fitted to measured caster depth bounds instead of a blind radius multiple | `fitDirectionalShadowCascadeSliceWithCasterDepthBounds()`, `ensureStaticCasterLightDepthBounds()` | Yes, through a new Z-only input rather than `use_scene_bounds` |
| Adaptive refresh budget, at most one cascade re-rendered per frame | `kStaticOnlyAdaptiveRefreshBudgetPerFrame` | Yes, for proactive refresh only |
| Occlusion test that projects bounds through `previousViewProj` instead of requiring a matching camera | `visibilityBoundsOccludedByPreviousDepth()` in `visibility_common.sp` | Predictor only; it cannot authorize a VKR reject under motion |
| Two-channel min and max depth pyramid | `depth_minmax_pyramid.frag` | Technique only; VKR starts with occupied-depth reduction so background is excluded |
| Same-frame depth used to confirm rather than authorize a reject | `opaque_meshlet_current_hiz.sp` | Required for moving-camera rejection |
| SDSM from a previous-frame depth pyramid min/max, GPU reduce, ring readback, temporal blend | `shadow_sdsm_prev_frame_minmax.comp`, `SdsmState`, `consumeRawDeviceMinMax()` | Yes, with source projection and completion metadata |
| Rotated Poisson PCF with a uniform-region early out | `poissonPcfDirectionalShadowFactor()` in `material_lighting.sp` | Yes, as a quality restore, gated on GPU measurement |
| Cascade blend band and max-distance fade | `makeDirectionalShadowCascadeState()`, `fadeParams` | Yes |
| CPU light-space uniform grid over static casters, 64x64x16 cells | `staticLightGrids_`, `StaticShadowCasterLightGridCell` | No. This accelerates CPU per-cascade caster culling that our GPU classify already does |
| CPU batch templates, instance remap buffer, indirect packet assembly | `StaticShadowBatchTemplate`, `buildShadowDraws()` | No. Our compaction pass produces the same thing on the GPU |
| Per-cascade separate depth textures | `shadowDepthTextures_[kMaxShadowCascades]` | No. Our single array image with per-layer barriers is better for us |
| Pending-frame transaction and rollback | `PendingFrameState`, `restorePendingFrameState()` | Required for retained fit and graph-state commit |

Two limits matter. `buildShadowDraws()` is roughly 1700 lines and carries CPU
bookkeeping that VKR's GPU compaction does not need. Nuri's SDSM tail read also
does not prove the reduced maximum excludes clear-depth background. Its code is
reference material, not performance or correctness evidence for VKR.

## 4. Phase 0, measurement and instrumentation

**Status:** Implemented. Authoritative clean-tree profiles for the actual
reported workload remain open.

Phase 0 is its own instrumentation PR. No optimization or quality preset change
belongs in that PR. The gate must run on a clean tree in Release and must include
the scene and camera path where the CPU problem was reported. The existing
static Bistro cases do not substitute for that moving-camera workload. If no
matching case exists, add `tools/cases/performance/bistro_shadow_orbit.case.json`
or a case named for the actual reproduction before collecting the baseline.

Run the CPU and GPU profiles separately:

```sh
./build_release.sh
./build_release/tools/vkr_harness profile \
  --case tools/cases/performance/sponza_orbit.case.json \
  --profile tools/profiles/performance-windowed.json
./build_release/tools/vkr_harness profile \
  --case tools/cases/performance/sponza_orbit.case.json \
  --profile tools/profiles/performance-windowed-gpu.json
```

Repeat both commands with the reported workload case. Run both selected
implementations where the platform is available. Metal and Vulkan perform
parallel candidate hashing and packing, so Vulkan-only scopes cannot authorize a
shared frontend rewrite.

Record from the aggregate `report.json` and `summary.csv`:

1. `cpu_ms` per pass for all four `Shadow.Cascade.*` passes, `Cull.Classify`,
   `Cull.Prefix`, `Cull.Encode`, and `Cull.Upload`.
2. `gpu_ms` with `gpu_valid` for the same passes, from the GPU profile only.
3. `cpu.frame_work`, `cpu.render_prepare`, and `cpu.render_submit`.
4. `draw.shadow.cascadeN.indirect_commands`, `.indirect_calls`, and
   `.indirect_overflow` for each active cascade.
5. `visibility.hzb.history_valid` and `visibility.hzb.rejected` per frame, to
   confirm or refute section 2.4.
6. `VkrVisibilityStats.objects_tested` and the candidate count.

Add backend-neutral CPU scopes before running. Register the same metric names
for both selected implementations:

- `cpu.shadow_update`, around `vkr_shadow_system_update()`.
- `cpu.world_payload_build`, around `application_build_world_payload()`.
- `cpu.packet_candidate_hash`, around the backend candidate epoch calculation.
- `cpu.packet_candidate_pack`, around opaque and transmission candidate packing.
- `cpu.packet_geometry_table_build`, around geometry-table clear and rebuild.

Also publish bytes written for candidate rows, instance rows, and geometry rows.
Timing without work volume cannot distinguish a cache win from silently omitted
draws.

Acceptance requires `status=pass`, `authoritative=true`, an empty
`authority_reasons` array, at least two independent repetitions, matching
environment, workload, and policy fingerprints, stable warmup, and identical
work-volume rows across repetitions. Record the report SHA-256 and transcribe
the decisive values before deleting the run tree.

The gate decides the order of work:

| Measurement | Consequence |
|---|---|
| `cpu.world_payload_build` plus candidate hash/pack is at least 5% of `cpu.frame_work` or misses the CPU budget | Authorize Phase 2 |
| Shadow pass GPU time dominates while CPU scopes are flat | Do not call this a CPU fix; authorize the retained-history ADR and Phase 3 investigation |
| HZB history is invalid on moving frames and rejected work would be material | Authorize the two-phase visibility ADR investigation, not a one-phase reject |
| `cpu.shadow_update` is at least 5% of `cpu.frame_work` | Stop and re-profile the function before changing candidate ownership |
| No named scope is at least 5% and no budget is missed | Stop after Phase 1; this proposal has not found the reported limiter |

The 5% threshold is an authorization threshold, not a promised speedup. Owner
approval may replace it with an explicit millisecond budget for the target
machine, but that budget must be written into the report before Phase 2 starts.

## 5. Phase 1, make the current contract truthful

**Status:** Implemented. Exact unchanged-default capture comparison remains
open.

Phase 1 may land after Phase 0 even when no optimization is authorized. It must
not delete fields that Phase 7 will immediately restore or change quality
presets under the name of cleanup.

Classify every `VkrShadowConfig` and `VkrShadowFrameData` field in the header as
one of:

- consumed now, with the consumer named in its comment;
- reserved for Phase 7, documented as inactive and kept out of the packet ABI
  until the consumer ships;
- obsolete, removed from both structs and every preset in the same PR.

Inactive Phase 7 fields stay in `VkrShadowConfig` so external configuration does
not churn, but stay out of `VkrShadowFrameData` until their lowering ships. The
per-frame structure contains only data consumed by the current packet path.

Route the existing `depth_bias_*` values through `VkrShadowConfigOverride` to
both backends instead of retaining hardcoded Vulkan-only defaults. Keep raster
depth bias separate from receiver bias. Do not expose a control on one backend
until the other backend has the same semantics.

Keep `world_units_per_texel`, `light_space_origin`, and the fitted light-space
depth span in cascade data. Phase 3 needs the first value for guard-band math;
Phase 7 needs all three for stable filtering and unit-correct bias.

Add hysteresis to cascade fitting, ported from `shadow_math.h`:

- Quantize the ortho extent up to a texel multiple before deriving
  `texelWorldSize`, then re-derive. See `quantizeShadowExtentUp()`.
- Keep the previous frame's snapped light-space center when the new unsnapped
  center is within one texel of it. See `applyDirectionalShadowFitHysteresis()`
  and `kShadowStabilizationCenterHysteresisTexels`.
- Keep the previous extent when the new one is smaller by less than two texels,
  and the previous depth range when the new one shrinks by less than two. See
  `keepAnchorShadowExtentWithinDeadband()` and
  `keepAnchorShadowDepthWithinDeadband()`.

This adds one field to `VkrShadowSystem`, a `VkrShadowFitHistory` holding
the previous frame's per-cascade fit plus the light direction, cascade count,
map size, projection convention, and shadow enable generation for which it is
valid. Invalidate on any of those changing, on scene replacement, and after a
frame gap caused by target recreation. Unit tests must cover growth, shrink
deadbands, light-direction invalidation, and disable/re-enable.

Do not change `cascade_split_lambda`, `shadow_map_size`, or
`max_shadow_distance` in Phase 1. Lambda 0.25 trades near density for far
density, and 4096 raises both memory and raster cost. Evaluate those as named
quality experiments after Phase 7 has a stable filter. A 4096 D32 array costs
256 MiB per physical image, or 768 MiB at three target images.

## 6. Phase 2, versioned candidate residency

Phase 2 is authorized only when Phase 0 finds material CPU cost in payload
construction or backend candidate packing. It is a world-renderer throughput
change, not a shadow subsystem change, and must preserve one candidate source
for camera, transmission, picking, and cascades.

### 6.1 Frontend ownership and mobility

Add a renderer-owned `VkrWorldCandidateCache`. Allocate its reloadable arrays
from `VkrDMemory`, not the frame scratch arena or a grow-only scene arena. Free
them on renderer shutdown and replace their contents transactionally on scene
reload.

Every mesh instance gets explicit shadow mobility:

```c
typedef enum VkrShadowCasterMobility {
  VKR_SHADOW_CASTER_MOBILITY_DYNAMIC = 0,
  VKR_SHADOW_CASTER_MOBILITY_STATIC = 1,
} VkrShadowCasterMobility;
```

Unknown and runtime-created instances default to `DYNAMIC`. The scene loader
may mark an instance `STATIC` only when its transform, geometry, deformation,
and material routing cannot change without going through an invalidating API.
"Has not moved recently" is not a static classification.

The cache stores two contiguous backend-neutral arrays, static and dynamic.
Static candidates rebuild on a cache generation change. Dynamic candidates
rebuild every frame. Blend visibility and depth sorting remain per frame.

### 6.2 Generations and invalidation

`VkrWorldCandidateCache` owns monotonic 64-bit generations. Zero is invalid and
wrap is fatal in Debug. Mutators bump generations at the cold boundary, before
the next packet is built.

| Change | Required invalidation |
|---|---|
| instance add/remove, visibility, loaded state, mobility | topology and static/dynamic content |
| static transform change | reject the mutation or reclassify dynamic before publication; then topology and static content |
| dynamic transform or object ID | dynamic content |
| mesh/submesh, LOD, geometry handle, bounds, deformation/skinning | affected content and caster bounds |
| material handle, alpha routing, transmission, double-sided state, alpha texture | affected content and pipeline signature |
| geometry/material publication or retirement | publication generation |
| scene replace/unload | all generations and cache identity |

No caller may mutate a cached fact by writing a public struct field directly.
The Phase 2 PR must route existing mutations through manager functions or keep
the old rebuild path for that object until the mutation seam is closed.

### 6.3 CPU build contract

Rebuild the static array in one count-and-emit operation when its generation
changes. Exact pre-counting or fixed-capacity storage is allowed; repeated
growth during emission is not. Build the dynamic array once per frame. Preserve
the current conservative rule that off-camera candidates may cast into a
cascade.

The packet carries borrowed spans plus their generations:

```c
typedef struct VkrWorldCandidateSpans {
  const VkrWorldDrawCandidate *static_candidates;
  uint32_t static_count;
  uint64_t static_generation;
  const VkrWorldDrawCandidate *dynamic_candidates;
  uint32_t dynamic_count;
  uint64_t dynamic_generation;
  uint64_t publication_generation;
} VkrWorldCandidateSpans;
```

The cache owns the static span. The frame scope owns the dynamic span. Both stay
alive until `vkr_renderer_submit_packet()` returns. Their combined count must
fit `VKR_GPU_DRAW_CANDIDATE_CAPACITY`; packet validation reports overflow before
command recording.

### 6.4 GPU storage and in-flight safety

Keep the current per-frame-slot candidate and instance buffers. Each slot stores
the static and publication generations last packed into that slot. A slot may
be rewritten only after its existing completion proof has passed, using the
same frame-slot rule as command and upload storage.

When a completed slot is acquired:

1. repack its static range when either cached generation differs;
2. pack the dynamic range immediately after the static range every frame;
3. publish the combined count and static boundary for that slot;
4. record the candidate copies required by those dirty ranges;
5. clear compaction state every frame, even when no candidate bytes changed.

Do not make `Cull.Upload` conditional. Vulkan's `vkCmdFillBuffer()` and the Metal
equivalent reset are mandatory per-frame work. Only the candidate copy ranges
may be empty. This design uploads a new static generation at most once per frame
slot, avoids rewriting buffers used by an in-flight frame, and keeps one
contiguous shader input without a per-candidate static/dynamic branch.

The frontend cache must not store Vulkan addresses, Metal resource IDs, or
resolved descriptor rows. Each selected implementation owns its packed rows and
generation stamps. Vulkan and Metal must expose the same counters and omission
semantics.

### 6.5 Deferred publication

Packing resolves geometry and material handles against the selected
implementation. An unpublished dependency omits that candidate from the slot's
packed range for the current frame and records its source index in a bounded
pending set. Completion or retirement increments `publication_generation`, so
every completed slot retries affected candidates. A pending list is an
optimization, not the sole invalidation mechanism.

Tests must cover publication after omission, retirement and republish with a
new handle generation, scene unload with pending entries, and capacity
overflow. A candidate may disappear temporarily while its dependency is
unpublished; it may not remain absent after the publisher reports it ready.

The fixed geometry table is a separate cost. Instrument it in Phase 0. Do not
fold geometry-table residency into this slice unless its own scope crosses the
authorization threshold.

### 6.6 Caster-depth snapshot

While rebuilding the static cache, accumulate a world-space AABB for static
shadow casters. Cache its light-space depth interval by static generation and
light direction. Scan only dynamic caster bounds each frame and union their
light-space interval with the cached static interval.

Pass the result as an optional `VkrShadowCasterDepthBounds` input to
`vkr_shadow_system_update()`. This input changes only the cascade Z interval.
Do not set `VkrShadowSceneBounds.use_scene_bounds`, because that path also
changes light-view anchoring and XY fit. When the snapshot is invalid or empty,
retain `z_extension_factor` as the fallback.

The cache key includes static generation, bounds generation, normalized light
direction, and projection convention. CPU tests cover invalidation and empty
scenes. Exact shadow-factor captures must show that the tighter interval does
not clip off-camera casters before the fallback tuning fields are deprecated.

## 7. Phase 3, retained shadow history and static-only reuse

Phase 3 has two PRs. Phase 3A adds retained graph-resource semantics and renders
all cascades exactly as before. Phase 3B uses that contract to omit eligible
cascade passes. Do not combine them.

### 7.1 Phase 3A, retained image semantics

Physical image allocation reuse does not preserve graph contents. The compiler
currently seeds every non-imported image as `UNDEFINED`, and `shadow_map` rotates
across `PER_IMAGE` instances. Add a `RETAINED` graph resource flag with these
rules:

- `RETAINED` is graph-owned, non-aliasable, and mutually exclusive with
  `TRANSIENT` and `EXTERNAL`.
- `PER_IMAGE` still creates one physical instance per target image.
- The selected implementation stores terminal access, stages, layout, and a
  content-valid bit beside each realized physical instance and subresource. The
  per-frame graph carries only the selected seed and pending terminal state.
- Compilation receives the selected resource-instance index and its seed, then
  initializes retained subresources from the last successfully submitted state
  for that instance.
- Reading a retained subresource whose content-valid bit is false is a compile
  error. The frame must schedule a writer instead.
- A successful submit commits terminal states and content-valid writes. A
  skipped, rejected, or failed frame does not commit them.
- Resize, format/layer/mip change, target recreation, image-count change, or
  graph generation change invalidates contents. Physical destruction still
  waits for proven GPU completion.
- A later submit may rely on same-queue ordering with the recorded submit value;
  CPU code must not block waiting for the older contents. Cross-queue use needs
  an explicit semaphore dependency before it is supported.

Change `shadow_map` to `RETAINED`, `PER_IMAGE`, and `RESIZABLE` only after both
selected implementations persist and seed the same subresource state. Add
compiler tests for invalid read-before-write, per-image separation, per-layer
state, resize invalidation, failed-frame rollback, and successful commit. Run a
focused validation case across at least two and four target images.

This contract requires a proposed render-graph ADR because it changes the
meaning of graph-owned resources across frames.

### 7.2 Per-image cascade state and frame ordering

`VkrShadowSystem` keeps raw fits independent of a target image. Prepare returns
the selected image index plus a backend-neutral retained-shadow token containing
the shadow resource generation and valid-layer mask. After
`vkr_renderer_prepare_frame()`, resolve reuse for that physical shadow instance:

```c
typedef struct VkrShadowCascadeHistory {
  bool8_t static_only_contents;
  VkrShadowFit rendered_fit;
  uint64_t static_generation;
  uint64_t caster_bounds_generation;
  uint64_t bias_signature;
  uint64_t light_signature;
  uint64_t resource_generation;
  uint64_t last_submit_value;
} VkrShadowCascadeHistory;
```

Store one record per target image and cascade. `vkr_shadow_system_update()`
continues to compute raw fits before prepare. A new
`vkr_shadow_system_resolve_frame(image_index, retained_token,
candidate_snapshot, ...)` returns the render mask and the matrices that
receivers must use. The retained token is the authority for content validity;
the shadow history owns only the fit and caster signatures associated with that
resource generation. The existing `vkr_shadow_system_get_frame_data()` must
stop ignoring its image index.

Stage history updates in a pending record. Commit them only after
`vkr_renderer_submit_packet()` succeeds and returns the submit serial. Discard
them on packet rejection, skipped submission, or device error. Target
recreation invalidates every record before any new frame resolves reuse.

### 7.3 Reuse rule

A layer may be reused only when all conditions hold:

1. retained graph contents are valid and marked static-only for the selected
   physical image and layer;
2. static candidate, caster-bounds, light, bias, format, and resource
   generations match the rendered record;
3. the rendered fit contains the current raw fit on all six light-space sides;
4. every dynamic shadow candidate has valid bounds and none intersects the
   guarded rendered volume;
5. no pending or omitted publication could add a caster to that volume.

Phase 2 supplies a bounded dynamic span. Resolve overlap on the CPU by
transforming each dynamic bounding sphere into light space once and testing all
active cascade volumes. If any dynamic candidate lacks bounds, exceeds the
configured scan budget, or has an unpublished dependency, force all possibly
affected cascades to render. Record dynamic candidates tested and cascades
forced as metrics.

When a layer is reused, publish its `rendered_fit.view_projection`, not the raw
fit. When a layer renders, use and publish its new guard-banded fit and stage
that fit for commit. Mark the new contents static-only only when the overlap
scan found no dynamic caster and no candidate was omitted or pending. A layer
last rendered with dynamic content must render once after those casters leave
before it can become reusable; otherwise their old depth would remain as a
stale shadow.

### 7.4 Guard band and refresh policy

Port Nuri's guard-band and predictive-motion math as a pure CPU module with unit
tests. Treat Nuri's one-eighth XY and one-sixteenth depth guards as starting
values, not defaults guaranteed to ship. Expose per-cascade guard texels and a
maximum predictive term. Clamp expansion before matrix construction.

The refresh budget applies only to proactive refreshes. A layer that fails any
correctness condition renders even if that exceeds the budget. With a proactive
budget of one, choose the valid reusable layer with the least remaining guard
margin. Record rendered, reused, correctness-forced, and proactive-refresh
counters per cascade.

### 7.5 Graph topology

Add optional `condition_mask_source` to repeated JSON passes. The graph builder
evaluates bit `repeat_index` at the cold build boundary. Both implementations
must consume the same `shadow_cascade_render_mask`; an executor early return is
not allowed because attachment load/store actions and barriers would already be
wrong.

Downstream passes still declare a sampled read of every active layer. A rendered
layer transitions from depth attachment to sampled. A reused layer starts from
its retained sampled state and remains sampled. Update
`docs/rendering/render-graph-schema.json` and its parser/compiler tests in the
same PR.

### 7.6 Memory and deferred variants

The initial implementation keeps one D32 array and one common extent. An array
cannot use different formats or dimensions per layer. At 2048 and four layers,
retained shadow memory remains 64 MiB per target image. At 4096 it is 256 MiB per
target image.

Do not add the static-cache plus dynamic-overlay copy in Phase 3. That variant
needs a second retained array, `TRANSFER_SRC` on the cache, `TRANSFER_DST` on the
working image, a `LOAD` dynamic pass, and matched bandwidth evidence. At 2048
D32 it adds another 64 MiB per cache instance and can move about 32 MiB per
copied layer. It gets a separate design and authorization after basic reuse is
measured.

Changing all cascades to D16 is another separate experiment. Per-cascade format
or resolution requires separate images or an atlas and is outside this design.

## 8. Phase 4, correctness-preserving temporal occlusion

Deleting the exact-matrix or world-epoch gates and authorizing rejection from
previous depth is not safe. Camera motion can reveal a static object, and a
moving occluder can reveal a static object. Bounds-inside guards prevent invalid
projections; they do not prevent disocclusion.

The existing one-phase path keeps its exact history gates. Moving-camera reuse
requires a two-phase topology and a proposed amendment to ADR-028.

### 8.1 Previous HZB is a predictor

Project candidate bounds through the matrix that produced the previous HZB.
VKR's Vulkan projection uses NDC Z in `[0, 1]`; do not copy Nuri's `[-1, 1]`
guard. Bail to probable-visible when any corner crosses the previous near plane,
falls outside `[0, 1]`, leaves the HZB UV rectangle, has invalid bounds, or uses
an incompatible projection convention.

The predictor partitions frustum-visible candidates into:

- probable-visible, rendered in the first raster phase;
- deferred, which previous depth predicted as occluded.

Dynamic candidates bypass prediction and enter probable-visible. This improves
first-phase depth and reduces confirmation work, but correctness still comes
from section 8.2.

### 8.2 Current-depth confirmation

The graph topology becomes:

1. classify and encode probable-visible candidates;
2. `VBuffer.Opaque.Initial`, clearing depth and visibility;
3. build `HZB.Provisional` from that current-frame depth;
4. classify the deferred set against `HZB.Provisional` using the current
   view-projection;
5. encode survivors into supplemental indirect buckets;
6. `VBuffer.Opaque.Confirm`, loading depth and visibility and drawing survivors;
7. run G-buffer resolve and downstream shading only after confirmation;
8. build or refresh the final history HZB from the completed depth when SDSM or
   the next frame requires complete depth.

The provisional test may fail open and draw extra work. It may not reject a
candidate whose current projected bounds are outside the valid HZB domain.
Frustum-visible candidates rejected by previous history reach either the
confirmation draw or a current-depth occlusion proof. No previous-frame result
is the final authority.

Compaction state, overflow counters, and visible-table ranges are separate for
initial and confirmation phases. Supplemental commands use the same pipeline,
material, and geometry compatibility rules as the primary buckets. G-buffer
resolve and picking see the union.

This topology applies to camera opaque and cutout candidates. Transmission runs
after confirmed opaque depth exists and may test that current HZB directly. If
the implementation instead uses previous depth for transmission, it keeps the
exact history gates until it has an equivalent confirmation path.

### 8.3 Evidence and stop condition

Add counters for predicted-deferred, confirmation-tested,
confirmation-survived, current-depth-rejected, and overflow. Exact moving-camera
captures must exercise disocclusion behind both static and moving occluders.
Vulkan and Metal validation must cover the extra depth `LOAD`, visibility-buffer
writes, barriers, and indirect state.

Ship only when matched Release profiles show a net win after the second classify,
raster, and HZB work. If confirmation overhead consumes the saved work, retain
the exact-gated one-phase path and close this phase without changing correctness
policy.

### 8.4 Cascade occlusion is deferred

Do not add one-phase previous-HZB rejection to shadow cascades. It has the same
disocclusion problem and would add up to four reduction chains. Phase 3 pass
omission is the first cascade optimization. A later cascade-HZB proposal must
define a current-depth confirmation topology per rendered cascade and beat
reuse in matched GPU evidence.

Do not widen the existing R32 max HZB to RG32 merely to share it with SDSM.
Section 10 defines an occupied-depth reduction. Compare its measured bandwidth
against a shared representation before merging the resources.

## 9. Phase 5, optional proactive refresh scheduling

Staggering is not independent of Phase 3. It uses the retained image, per-image
history, guarded fit, dynamic-overlap test, pending commit, and rendered-fit
publication rules from that phase.

The only authorized baseline is proactive refresh scheduling for layers that
remain valid for reuse. Assigning a fixed stale interval to a layer with a
dynamic caster is prohibited. A correctness-forced render ignores the schedule.

Start with the Phase 3 budget of one proactive refresh per frame. Add longer
per-cascade intervals only if metrics show guard exhaustion still clusters
work. Keep the feature off by default until a moving-camera profile proves a
lower p95 without increasing correctness-forced refreshes or changing exact
shadow captures outside the deliberately enlarged guard fit.

## 10. Phase 6, occupied-depth SDSM feedback

SDSM is a separate proposed ADR because it adds delayed GPU-to-CPU feedback to
frontend-owned cascade fitting. It is a quality and distribution feature, not a
proven CPU optimization.

### 10.1 Reduction source

Do not reduce raw depth as plain min/max. Clear-depth background would keep the
maximum at the far plane in any view containing sky. Build an occupied-depth
reduction from final opaque depth plus the visibility ID:

```c
typedef struct VkrSdsmReduceValue {
  float32_t min_device_z;
  float32_t max_device_z;
  uint32_t occupied_count;
} VkrSdsmReduceValue;
```

The base pass writes `occupied_count = 0` for background and one for a valid
visibility ID. A reduction combines min and max only from children with nonzero
count and sums the count with saturation. An empty tail is invalid. Keep this
chain separate from the R32 max HZB for the first implementation; merge them
only if a GPU profile proves the wider shared chain is cheaper.

### 10.2 Completed feedback record

The selected implementation writes one fixed-size result per frame slot:

```c
typedef struct VkrShadowDepthRangeSample {
  float32_t min_device_z;
  float32_t max_device_z;
  uint32_t occupied_count;
  uint32_t projection_convention;
  Vec4 source_depth_linearize;
  float32_t source_near;
  float32_t source_far;
  uint64_t source_frame_index;
  uint64_t source_projection_generation;
  uint64_t source_scene_generation;
  uint64_t submit_value;
  bool8_t valid;
} VkrShadowDepthRangeSample;
```

Store the source projection parameters with the result. Never linearize delayed
device depths using the current camera projection. The backend exposes only the
newest slot whose submit value is complete and whose stored frame identity still
matches the slot record. No frame path waits for a fence, semaphore, queue, or
readback slot.

`vkr_shadow_system_update()` accepts an optional completed sample. The frontend
rejects it after target recreation, projection or scene generation change,
non-finite or unordered depths, zero occupied pixels, or a configured
source-frame lag. Camera pose changes do not invalidate it; delayed visible
depth is the input SDSM is designed to consume. Linearize through the stored
source coefficients, clamp to the fixed shadow range, and apply temporal
smoothing in linear view distance.

### 10.3 Split and fallback policy

SDSM adjusts the working near/far interval used to distribute the interior
splits. The final cascade still reaches the configured fixed shadow distance so
visible receivers do not lose all directional shadow merely because they were
absent from the delayed depth sample. Clamp interval contraction per frame and
run it through Phase 1 hysteresis.

Use the newest valid cached sample only up to `sdsm_max_source_lag_frames`.
During warmup or after invalidation, use fixed splits. Missing feedback is normal
and never an error. Publish `warmup`, `active`, `cached`, `empty`, `stale`, and
`fixed_fallback` status plus source lag, occupied pixels, and the linear range in
frame metrics and shadow debug data.

Unit tests own device-depth linearization, empty/background rejection, stale
sample rejection, projection changes, smoothing clamps, and fixed-last-split
coverage. Validation owns buffer barriers and completion. Exact captures own
split stability; matched GPU profiles decide whether the reduction is affordable.

## 11. Phase 7, PCF and receiver quality

This phase adds GPU cost and ships independently of the CPU work. Its baseline
is the current one-tap receiver at the same map size and split distribution.

### 11.1 Payload and ABI

`VkrShadowPassPayload` carries one backend-neutral block per cascade:

```c
typedef struct VkrShadowCascadePacketData {
  Mat4 light_view_projection;
  Vec4 split_near_far_texel_depth; // near, far, world units/texel, Z span
  Vec4 origin_inv_size_pad;         // light-space origin xy, inverse map size
} VkrShadowCascadePacketData;
```

Shared state contains `receiver_bias_texels`, `slope_bias_texels`,
`normal_offset_texels`, `pcf_sample_count`, `pcf_radius_texels`,
`cascade_blend_fraction`, `fade_start`, and `fade_end`. Put shared state and the
cascade array in one backend-neutral packet block. Each selected implementation
lowers it once into its immutable frame upload. Do not consume reserved words
piecemeal until reflection proves the ABI on both backends. Bump the packet
version, update validation, and update the Metal and Vulkan ABI tests in the
same PR.

Validate `pcf_sample_count` against a fixed supported set before recording. The
shader hot path receives normalized data and has no recovery branches.

### 11.2 Filter

Use one shared 64-entry Poisson table in the shader sources and preserve the
lower-granularity one-tap path for the quality sweep.

- Rotate the kernel with the existing stable light-space cell hash derived from
  `light_space_origin` and `world_units_per_texel`. Nuri's four cascade-fixed
  rotations are an A/B candidate, not the default; the previous VKR design
  rejected their stationary patterning.
- A nine-tap uniform-region early out before the full kernel when the sample
  count is 16 or more. Fully lit and fully shadowed regions cost nine taps
  instead of sixteen or more. Measure early-out enabled and disabled with the
  same tap count.
- Vulkan publishes logical raw and comparison sampler heap indices through its
  frame root. Metal uses matching constexpr raw and comparison samplers. The
  backend-neutral packet contains filter settings, never backend sampler IDs.

### 11.3 Bias

Bias has five separate controls and units:

| Control | Unit and lowering |
|---|---|
| raster constant bias | backend depth-bias units, applied while rendering the shadow map |
| raster slope bias | backend slope factor, applied while rendering the shadow map |
| receiver depth bias | texels converted to normalized orthographic depth as `receiver_bias_texels * world_units_per_texel / light_space_depth_span` |
| receiver slope bias | texels multiplied by a clamped function of `1 - N dot L`, then converted through the same depth span |
| normal offset | world units, `normal_offset_texels * world_units_per_texel`, applied before light projection |

Do not multiply an already normalized NDC constant by world-units-per-texel.
Remove `vkr_packet_shadow_bias` only when both receiver shaders consume the new
block. Nuri's values are starting points for captures, not compatible defaults.
Tune after caster-depth fitting stabilizes the Z span.

### 11.4 Blending and fade

Cross-fade between cascade `c` and `c+1` over the last
`cascade_blend_fraction` of cascade `c`'s span. Outside that band, sample one
cascade. Fade shadow strength to zero from `fade_start` to `fade_end` before the
fixed final split. Packet validation requires ordered, finite ranges and clamps
the blend fraction at the cold boundary.

### 11.5 Cost control

Run matched GPU profiles at 1, 4, 9, 16, and 32 taps, plus the uniform-region
early-out A/B at 16 and 32. Record `Lighting.Deferred` GPU time and work volume.
Choose the default from a written GPU budget and exact captures. Do not claim
the filter is paid for by a CPU optimization; report its cost independently.

Keep map size, split lambda, and tap count as separate experimental variables.
Changing more than one makes the capture and timing result uninterpretable.

## 12. Implementation slices and ADRs

One writer owns each slice. A slice lands only after its own acceptance row in
section 15 passes. Do not batch graph state, candidate lifetime, visibility
topology, and shader quality into one change.

| Slice | Primary owned files | Required rationale update |
|---|---|---|
| 0, instrumentation | `lib/src/application.h`, `vkr_renderer_metrics.c/h`, Vulkan draw packing, Metal packet frame/resources, performance case/profile manifests | none |
| 1, truthful shadow data and hysteresis | `vkr_shadow_system.c/h`, `vkr_render_packet.h`, application packet assembly, both packet implementations, shadow CPU tests | architecture spec feature row and old PCF status sentence |
| 2, candidate residency | mesh-manager mutation APIs, `application.h`, `vkr_render_packet.h`, Vulkan draw packing, Metal packet resources, publication tests | update ADR-004 and ADR-028 with split spans, ownership, and generation semantics |
| 3A, retained graph images | `vkr_render_graph.h`, `vkr_rg_compile.c`, `vkr_rg_execute.c`, `vkr_rg_json.c`, graph schema, both backend graph realizations, graph compiler tests | new proposed retained-resource ADR, accepted before implementation |
| 3B/5, reuse and refresh | `vkr_shadow_system.c/h`, application frame resolution, packet payload, graph JSON, both shadow executors, reuse tests/case | mark retained-resource ADR implemented or partial |
| 4, two-phase HZB | graph JSON, cull roots/shaders, both backend encoders, metrics, disocclusion case | new proposed ADR amending ADR-028's visibility topology |
| 6, SDSM | occupied-depth shaders, graph JSON, backend readback slots, shadow-system input/status, tests | new proposed GPU-feedback ADR |
| 7, PCF | shadow config and packet ABI, Vulkan and Metal receiver shaders/samplers, reflection tests, capture cases | architecture feature/status update when shipped |

Every implementation PR must update this document's phase status, the
architecture-spec feature table, and `docs/README.md` when its behavior ships.
Move this document to `partial` after the first production slice lands and name
the remaining gaps at the top.

## 13. Non-goals

- Point and spot light shadows. Directional only.
- Ray-traced or screen-space contact shadows.
- A camera depth pre-pass. `VBuffer.Opaque` already fills that role, and section
  2.4 explains why adding one would cost vertex work and CPU for no benefit.
- Virtual shadow maps. The page-table machinery is much larger than this
  proposal, and the guard-banded reuse in Phase 3 addresses the same resolution
  against cost pressure at a fraction of the complexity.
- Porting nuri's CPU light grid, batch templates, or instance remap buffer. Our
  GPU compaction already produces the equivalent.
- Moving shadow draw construction back to the CPU in any form.

## 14. Risks

- Retained history can sample discarded contents if compile state commits before
  submit success or survives resize. Transaction and invalidation tests own this
  failure.
- A reused layer paired with a raw rather than rendered matrix looks like a bias
  defect. Per-image history and exact layer captures own this failure.
- A frame slot can still be in GPU use when another slot becomes current.
  Candidate residency keys reuse to the slot's completion proof, never frame
  distance.
- Incomplete publication can make a candidate permanently absent. Publication
  generation tests and repeated load/unload runs own this failure.
- An unbounded dynamic-caster scan can move the cost back to the CPU. A scan
  budget fails closed by forcing renders and publishes its count.
- Two-phase visibility can lose work at supplemental-buffer capacity. Overflow
  is an explicit frame error or conservative draw fallback, never a silent drop.
- SDSM can collapse onto background or oscillate. Occupied-count rejection,
  source projection metadata, contraction clamps, and fixed-split fallback own
  those failures.
- PCF, 4096 maps, and static-layer copies can erase GPU gains. Each remains a
  separate measured variable, and memory reports multiply image bytes by the
  physical target-image count.

## 15. Evidence required per phase

| Phase | Gate |
|---|---|
| 0 | Clean authoritative CPU and GPU profiles for Sponza orbit and the actual reported workload; both backends where available; exact scopes, bytes, HZB validity, work rows, report digest, and stop decision recorded |
| 1 | Focused hysteresis/config tests, `./build_test.sh`, packet validation tests, and exact unchanged-default captures; no performance claim |
| 2 | Mutation/publication/overflow/load-unload tests, `./build_test.sh`, one focused Vulkan validation run, one strictly serial Metal validation run, then matched authoritative Release profiles with identical work volume and candidate bytes |
| 3A | Retained-state compiler tests, `./build_test.sh`, Vulkan validation at two and four target images, and one serial Metal validation run; no performance claim |
| 3B | Phase 3A gates plus static, moving-camera, dynamic-caster, resize, and forced-publication cases; exact shadow-layer/final captures; matched profiles with per-cascade reuse and forced-refresh counters |
| 4 | CPU tests for projection bounds, `./build_test.sh`, both backend validation runs, disocclusion captures behind static and moving occluders, overflow coverage, and matched profiles showing net gain after confirmation work |
| 5 | Phase 3B gates plus p95 comparison and proof that only proactive, still-valid layers were scheduled |
| 6 | Reduction/linearization/staleness tests, `./build_test.sh`, both backend validation runs, status metrics covering every fallback, exact split-stability captures, and matched GPU profiles |
| 7 | Packet ABI/reflection tests, `./build_test.sh`, cold/warm production pipeline-cache runs, both backend validation runs, exact quality captures, and matched GPU sweeps for every proposed default |

Correctness is a separate gate. See
[`.codex/skills/vkr-validation/SKILL.md`](../../.codex/skills/vkr-validation/SKILL.md).

## 16. Resolved decisions and measured choices

Resolved by this design:

- The current HZB is single-channel max depth. SDSM starts as a separate
  occupied-depth reduction.
- Repeated passes gain `condition_mask_source`; neither backend may hide pass
  omission in an executor.
- `shadow_map` cannot preserve contents under current `TRANSIENT` and
  `UNDEFINED` seed semantics. Phase 3A adds `RETAINED` before reuse.
- History and fits are per physical target image and cascade.
- Previous-frame HZB does not authorize a moving-camera reject. Phase 4 uses
  current-depth confirmation.
- One array keeps one format and extent. D16, per-cascade resolution, and an
  atlas are separate designs.
- Caster bounds enter shadow fitting through a Z-only input. They do not enable
  `VkrShadowSceneBounds.use_scene_bounds`.
- Existing quality config is retained until its consumer ships or the field is
  explicitly rejected as obsolete.

Measurements choose, but do not block the architecture:

1. whether Phase 2 crosses the CPU authorization threshold;
2. guard-band texels and proactive refresh budget;
3. whether a static-layer copy deserves a later design;
4. SDSM smoothing and source-lag limits;
5. PCF tap count, early-out policy, split lambda, map size, and any all-cascade
   D16 experiment;
6. whether `vkr_shadow_fit_relevant_caster_z()` retains a production caller
   after the cheaper Z-only interval ships.
