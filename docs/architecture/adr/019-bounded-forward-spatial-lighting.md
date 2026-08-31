---
status: implemented
updated: 2026-08-31
authority: adr
---

# ADR-019: Bounded forward spatial lighting

## Status

**Accepted** — implemented on 2026-08-04 and amended on 2026-08-05 after
owner-camera evidence invalidated receiver-level light lists. A 2026-08-30
amendment that added exact punctual-light influence AABBs is rejected and
reverted: later owner evidence showed visible colored slabs outside Bistro
while the indoor indirect leak remained.

## Context

The original forward PBR path exposed one camera-ranked 16-light array. Bistro
instantiates 72 glTF punctual lights, so camera motion changed table membership
and visibly switched stationary fixtures on and off. The first correction kept
a stable 128-light scene table and selected 12 references from each visible
submesh's receiver sphere.

That correction removed camera-ranked membership but was still spatially
incorrect. Bistro's material-merged submeshes cover large, disconnected parts
of the scene: one receiver sphere overlapped dozens of valid lights, while the
12-reference cap silently discarded the lights required by individual
fragments. Five owner cameras measured 44,200 influencing receiver/light pairs
and 34,305 discarded pairs. Equivalent fixtures therefore worked on one part
of a broad draw and failed on another.

Local reflection probes have the same broad-mesh problem, but their fixed
three-map descriptor contract and per-fragment volume weights already resolve
the final choice in the fragment shader.

## Decision

Import glTF directional, point, and spot nodes into scene ECS components.
Point/spot intensity is applied once as glTF luminous intensity with
inverse-square attenuation, optional smooth range cutoff, and authored cones.

Retain up to `VKR_MAX_SCENE_POINT_LIGHTS` (128) in stable render-ID order. Build
a fixed-capacity, camera-independent 3D world grid from that table:

- the grid has at most `VKR_POINT_LIGHT_GRID_MAX_CELLS` (384) cells and adapts
  its cell size from finite lights' conservative range spheres;
- each cell stores a complete 128-bit mask, so membership has no secondary
  overflow limit;
- finite lights mark every sphere-intersecting cell conservatively;
- exact distance, range, and spot-cone rejection remain fragment operations
  before BRDF evaluation;
- unbounded legacy polynomial lights use a separate 128-bit global mask; and
- the fragment maps world position to one cell, iterates only its set bits, and
  addresses the stable scene table.

The grid and masks rebuild during scene-light synchronization with fixed
storage. No allocation, descriptor mutation, lock, string lookup, or logging
is added to draw or fragment submission. Masks occupy `float4` uniform slots
and preserve their raw bits across the existing reflection type vocabulary;
Slang recovers them with `asuint()`.

The punctual-light GPU row remains four `float4` values (64 bytes). Exact
range/cone semantics are shared by the Metal and Vulkan forward, deferred, and
transmission paths. Scene-specific range corrections are validated and applied
at import, so they do not add an optional case to the GPU contract.

The previous three packed light-index words are removed from draw candidates.
The GPU instance record remains 80 bytes for ABI stability, with those tail
words reserved and zeroed. Metrics report scene-table selected/dropped counts
and grid cells, references, maximum lights per cell, and global lights.

For image-based lighting, the frame packet carries up to 16 ready bounded local
probes plus the global environment. The shader evaluates authored AABB/blend
influence per fragment, normalizes local weights, and gives the remainder to
the global map. Probe AABBs are blending supports, not visibility volumes.
Their transition regions must not cross visible receivers as a substitute for
room topology. Bistro therefore retains one coherent indoor probe; a rejected
three-box subdivision produced world-axis-aligned floor and ceiling bands in
the isolated indirect-diffuse channel.

## Rejected amendment: punctual influence AABBs

The rejected amendment intersected range spheres with authored world AABBs and
applied an exact inclusive AABB test in every punctual shader loop. It also
expanded the light row from 64 to 96 bytes.

This representation was rejected because it did not contain light by opaque
geometry. It replaced physically smooth range falloff with hard axis-aligned
planes. The planes could be hidden behind one partition for one camera, but a
later exterior camera saw them as large colored slabs. Meanwhile, diffuse
probe light still crossed walls because that contribution has a separate
visibility problem.

The renderer therefore does not support punctual influence boxes. Decorative
light reach may be calibrated by finite range at the scene-import boundary.
Arbitrary wall and furniture occlusion requires a shadow or another
visibility-carrying representation.

## Consequences

- Broad or disconnected submeshes no longer decide light membership for all of
  their fragments. Camera movement alone cannot toggle a stationary fragment's
  mask.
- Bistro retains all 72 lights with zero scene-table drops. With the corrected
  5 m colored-light ranges, the grid uses 363 cells, 1,247 references, and at
  most 43 candidates in one cell.
- The grid is bounded to 384 cells, but cell size grows instead of dropping
  references. Coarser cells increase exact fragment rejection work without
  changing correctness.
- The reflected PBR global block remains within the 16 KiB Vulkan baseline
  range. The grid adds 6 KiB of fixed mask storage rather than an unbounded or
  per-frame GPU allocation.
- The instance ABI stays 80 bytes, with its final 12 bytes reserved rather than
  carrying receiver-selected light indices.
- At most 16 ready local probes are frame-packed, and every packed probe is
  considered per fragment. A future shortlist must retain fragment-local
  correctness.
- Diffuse probe bounds cannot represent wall occlusion. Geometry-aware diffuse
  containment requires a visibility-carrying GI representation.
- Range and cone rejection cannot represent arbitrary opaque visibility.
  Lights that require wall or furniture shadows need a separately budgeted,
  retained punctual-shadow design.

The current Bistro counts are structural correctness evidence, not a timing or
speed claim.

## Alternatives considered

**Keep one camera-ranked table.** Rejected because camera translation changes
lighting on stationary geometry.

**Keep the 12-light receiver shortlist.** Rejected by owner-camera evidence:
stable selection is insufficient when one receiver spans many light regions.

**Raise the per-instance cap.** Rejected because any fixed object-level list
retains the wrong granularity and enlarges every instance record.

**Author hard punctual influence AABBs.** Implemented experimentally, then
rejected by the exterior Bistro regression. The boxes were visible and were
not an opaque-visibility solution.

**Use a full clustered compute pipeline.** Deferred. The conservative CPU-built
bitmask grid provides fragment-local membership within current buffer,
synchronization, and descriptor seams. Compute clustering remains a measured
scalability option; it would not itself add opaque visibility.

## Revisit when

- More than 128 retained scene lights are required.
- The measured fragment cost of coarse cells justifies GPU-built clusters or a
  storage-buffer light list.
- Dynamic lights make per-frame CPU grid construction material to frame time.
- Punctual shadows receive an explicit face/update budget and completion-safe
  retained-resource design.
- More than 16 frame-packed local probes are required, or their measured
  per-fragment cost justifies a spatial shortlist that preserves
  fragment-local correctness.
