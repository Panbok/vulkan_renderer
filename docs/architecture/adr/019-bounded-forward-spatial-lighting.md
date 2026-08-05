---
status: implemented
updated: 2026-08-05
authority: adr
---

# ADR-019: Bounded forward spatial lighting

## Status

**Accepted** — implemented on 2026-08-04 and amended on 2026-08-05 after
owner-camera evidence invalidated receiver-level light lists.

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

The grid and masks are rebuilt during scene-light synchronization with fixed
storage. No allocation, descriptor mutation, lock, string lookup, or logging is
added to draw or fragment submission. The masks occupy `float4` uniform slots
and preserve their raw bits across the existing reflection type vocabulary;
Slang recovers them with `asuint()`.

The previous three packed light-index words are removed from draw candidates.
The GPU instance record remains 80 bytes for ABI stability, with those tail
words reserved and zeroed. Metrics report scene-table selected/dropped counts
and grid cells, references, maximum lights per cell, and global lights.

For image-based lighting, each draw still binds two bounded local probes plus
the global environment. The shader evaluates authored AABB/blend influence per
fragment, normalizes local weights, and gives the remainder to the global map.
Probe resources retain scene/world-resource ownership, and scene-environment
probes share ready irradiance/prefilter maps instead of rebaking them.

## Consequences

- Broad or disconnected submeshes no longer decide light membership for all of
  their fragments. Camera movement alone cannot toggle a stationary fragment's
  mask.
- Bistro retains all 72 lights with zero scene-table drops. The corrected grid
  exposes the lamps that receiver-level selection had discarded.
- The grid is bounded to 384 cells, but cell size grows instead of dropping
  references. Coarser cells increase exact fragment rejection work without
  changing correctness.
- The reflected PBR global block is 15,488 bytes, within the 16 KiB Vulkan
  baseline range. The grid adds 6 KiB of fixed mask storage rather than an
  unbounded or per-frame GPU allocation.
- The Bistro grid uses 363 cells, 1,635 finite references, and at most 47
  candidates in one cell. A matched local/dirty Release observation measured
  World Opaque p50 at 93.243 ms after exact early rejection versus 109.528 ms
  before that optimization. Those two runs share workload/environment/policy
  fingerprints; the older receiver-list run does not share the workload
  fingerprint, so its 93.272 ms p50 is observational context only.
- The instance ABI stays 80 bytes, but its last 12 bytes are reserved rather
  than carrying light indices.
- Only two local probe volumes can influence one draw. Extra overlapping probes
  still require a measured descriptor/storage design.

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
- More than two overlapping local probes per draw are required.
