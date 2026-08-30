---
status: implemented
updated: 2026-08-31
authority: adr
---

# ADR-019: Bounded forward spatial lighting

## Status

**Accepted** — implemented on 2026-08-04, amended on 2026-08-05 after
owner-camera evidence invalidated receiver-level light lists, and amended on
2026-08-30 with authored room containment for punctual lights. Later same-day
evidence rejected tightly tiled diffuse-probe boxes whose transitions crossed
visible receivers.

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
  its cell size from the finite lights' conservative range bounds;
- each cell stores a 128-bit mask, so every retained scene light is representable
  and cell membership has no secondary overflow limit;
- finite lights mark every sphere-intersecting cell conservatively; exact
  distance, range, and spot-cone attenuation remain fragment operations, with
  zero-contribution range/cone rejection before BRDF evaluation;
- unbounded legacy polynomial lights use a separate 128-bit global mask; and
- the fragment maps its world position to one cell, iterates only set bits, and
  addresses the stable scene table.

An imported point or spot light may additionally carry one validated
world-space influence AABB resolved from an exact glTF node name in the owning
scene mesh's `gltf_light_overrides`. Missing bounds canonicalize to finite
`[-VKR_FLOAT_MAX,+VKR_FLOAT_MAX]` values. Grid construction intersects a finite
glTF range sphere with the AABB; bounded polynomial and range-less lights use
the AABB directly. Every Metal and Vulkan forward, deferred, and transmission
loop then applies the inclusive exact AABB test before distance, cone, square
root, or BRDF work.

The GPU row grows from four to six `float4` values (64 to 96 bytes). Its first
four vectors retain their prior semantics; vectors four and five carry the
canonical minimum and maximum. Host assertions plus Metal and Vulkan compiled
reflection validate the shared record. The bound is authored containment, not
geometry visibility: it prevents a light assigned outside a room from crossing
that room's partition, but objects inside one volume do not cast punctual
shadows.

The grid and masks are rebuilt during scene-light synchronization with fixed
storage. No allocation, descriptor mutation, lock, string lookup, or logging is
added to draw or fragment submission. The masks occupy `float4` uniform slots
and preserve their raw bits across the existing reflection type vocabulary;
Slang recovers them with `asuint()`.

The previous three packed light-index words are removed from draw candidates.
The GPU instance record remains 80 bytes for ABI stability, with those tail
words reserved and zeroed. Metrics report scene-table selected/dropped counts
and grid cells, references, maximum lights per cell, and global lights.

For image-based lighting, the frame packet carries up to 16 ready bounded local
probes plus the global environment. The shader evaluates authored AABB/blend
influence per fragment, normalizes local weights, and gives the remainder to
the global map. Current packet construction packs ready scene probes globally
for the frame; the older draw-bounds selector is not the packet authority.
Probe resources retain scene/world-resource ownership, and scene-environment
probes share ready irradiance/prefilter maps instead of rebaking them.
Probe AABBs are blending supports, not visibility volumes. Their transition
regions must not cross visible receivers as a substitute for room topology.
Bistro therefore retains one coherent indoor probe; its rejected three-box
subdivision produced world-axis-aligned floor and ceiling bands in the isolated
indirect-diffuse channel.

## Consequences

- Broad or disconnected submeshes no longer decide light membership for all of
  their fragments. Camera movement alone cannot toggle a stationary fragment's
  mask.
- Room or zone membership can now be authored without changing the stable
  table, grid capacity, render graph, or per-draw packet shape. Malformed,
  duplicate, ambiguous, unknown, directional, non-finite, and inverted glTF
  overrides fail scene preparation instead of silently becoming unbounded.
- Bistro retains all 72 lights with zero scene-table drops. The corrected grid
  exposes the lamps that receiver-level selection had discarded.
- The grid is bounded to 384 cells, but cell size grows instead of dropping
  references. Coarser cells increase exact fragment rejection work without
  changing correctness.
- The reflected PBR global block is 15,488 bytes, within the 16 KiB Vulkan
  baseline range. The grid adds 6 KiB of fixed mask storage rather than an
  unbounded or per-frame GPU allocation.
- With the 54 Bistro influence bounds, the grid uses 363 cells, 1,458 finite
  references, and at most 33 candidates in one cell, down from 1,635 and 47.
  Local Release observations were non-authoritative because warmup was
  unstable, repetition work volume differed, provenance was dirty, and GPU
  timing was disabled. The counts are structural work-volume evidence, not a
  speed claim.
- The instance ABI stays 80 bytes, but its last 12 bytes are reserved rather
  than carrying light indices.
- At most 16 ready local probes are frame-packed, and every packed probe is
  considered per fragment. A future draw-level shortlist requires measured
  evidence and must not reintroduce broad-receiver selection errors.
- Diffuse probe bounds cannot represent wall occlusion. Geometry-aware diffuse
  containment requires a visibility-carrying GI representation rather than
  tightly tiled AABBs.
- Punctual influence bounds do not provide arbitrary opaque visibility within
  a zone. Lights that require furniture or same-room wall shadows need a
  separately budgeted, retained punctual-shadow design.

## Alternatives Considered

**Keep one camera-ranked table.** Rejected because camera translation changes
lighting on stationary geometry.

**Keep the 12-light receiver shortlist.** Rejected by owner-camera evidence:
stable selection is not sufficient when one receiver spans many light regions.

**Raise the per-instance cap.** Rejected because any fixed object-level list
retains the wrong granularity and enlarges every instance record.

**Use a full clustered compute pipeline.** Deferred. A conservative CPU-built
bitmask grid provides fragment-local correctness within current buffer,
synchronization, and descriptor seams. Compute clustering remains a measured
scalability option.

## Revisit When

- More than 128 retained scene lights are required.
- The measured fragment cost of coarse cells justifies GPU-built clusters or a
  storage-buffer light list.
- Dynamic lights make per-frame CPU grid construction material to frame time.
- More than 16 frame-packed local probes are required, or their measured
  per-fragment cost justifies a spatial shortlist that preserves fragment-local
  correctness.
