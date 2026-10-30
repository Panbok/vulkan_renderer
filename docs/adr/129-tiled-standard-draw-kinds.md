---
status: implemented
updated: 2026-10-30
authority: adr
---

# ADR-129: Tiled Standard draw kinds

## Status

Accepted. Since 2026-10-20 the tiled pipeline shades Standard camera draws
that cannot be lightmapped with pipelines that compile out the lightmap path,
and those on non-terrain materials also the terrain layers. Other draws keep
the general pipelines. The desktop pipeline does not change.
[ADR-136](136-standard-detail-maps.md) adds kinds 2 and 3 for the draws of
materials with detail maps, in compaction slots 35 and 36.

## Context

After [ADR-128](128-tiled-two-phase-occlusion.md), the slowest frames of the
Level Design Test recording `rec-20261019-231623` were the freight line's
outdoor views: `Tiled.Opaque` about 5.3 ms, fragment-bound (a Metal System
Trace at frame 10180 gave 3.9 ms fragment, 1.6 ms vertex). GPU counters over
that pass's fragment phase showed fragment occupancy at 17%, the ALU limiter
at 46%, the texture sample limiter at 9%, read bandwidth at 8 GB/s and no
compiler spill: latency-bound at low occupancy.

Removing work barely moved the pass; removing code did. Skipping the
diffuse volume's lookups saved 0.10 ms of its mean, compiling the volume out
0.21 ms of the mean and 0.45 ms of its p95. Compiling out the terrain layers
or the lightmap path each cut its p95 by 0.45 to 0.7 ms. One forward
pipeline carried every material path, so its register allocation was set by
code most draws never run ([tiled register pressure](087-gpu-class-graphics-pipelines.md#light-shading-variants)
already made light code a per-frame variant).

## Decision

1. **Kinds.** A Standard camera draw that cannot be lightmapped is of kind 0
   on a non-terrain material and kind 1 on a terrain material. A draw can be
   lightmapped when its instance has a lightmap slot and its geometry has
   lightmap UVs, the test the forward vertex applies; such draws, and Custom
   graph draws, keep the general pipelines.
2. **Pipelines.** Function constants 2 (`no_lightmap`) and 3 (`no_terrain`)
   in [tiled.metal](../../renderer/src/shaders/metal/msl/world/tiled.metal)
   compile the lightmap path, and the terrain layers, out of the forward and
   alpha-tested fragments. Undefined, they keep both paths. Only fragment
   code reads them: a vertex function that read them would need constants
   in every pipeline built from it. Kind 0 sets both, kind 1 only
   `no_lightmap`.
3. **Creation.** The frame's upload plan asks for the kinds of its shading
   variant (sun, and the BRDF extension plane when it buckets extended
   draws). A kind variant first asked for is created on the job system
   ([vkr_metal_packet_custom.inc](../../renderer/src/metal/internal/vkr_metal_packet_custom.inc),
   `vkr_metal_packet_standard_kind_mask`); until READY its draws keep the
   general pipelines. Each kind variant is 56 pipeline states.
4. **Routing.** The kinds take compaction slots 33 and 34 of
   `VkrMetalCustomCompaction`, after the Custom graphs'
   ([ADR-096](096-custom-material-graphs.md)), in both BRDF planes
   ([ADR-113](113-brdf-material-extensions.md)). The classify kernel marks a
   kind draw with classification bit 6 and the kind in bits 27 to 31, which
   a Standard draw leaves free; the encode kernel clears them. The table's
   `kind_ready_mask` holds bit `plane * 2 + kind` for the frame's ready kinds.
5. **Placement.** The prefix kernels place only the compaction ranges a pass
   draws: plane 1's slot 0 when the extension variant is ready, Custom slots
   in their plane's ready mask and ready kinds. The table starts zeroed, so
   other ranges stay empty.
6. **Diagnostic.** `VKR_STANDARD_KINDS_DISABLED=1` keeps every draw on the
   general pipelines, except detail draws, whose maps only their kinds
   draw (ADR-136).

## Consequences

- Kind draws shade with fewer live registers; their output is the general
  pipelines' for the same draw, since the removed code never runs for them.
- Frames that have a kind draw allocate the compaction table (8,992 bytes)
  and the late view's copy. The passes execute two more ranges per bucket
  and BRDF plane and bind the kinds' pipelines: on the recording,
  `Tiled.Opaque.Late` rose 0.023 ms, `Cull.Prefix` and `Cull.Late` about
  0.017 ms each.
- Startup is unchanged; each kind variant costs a background build of about
  0.1 s for the base variant and 0.24 s with the sun variant, and pipeline
  memory for 56 states.

## Alternatives considered

- **A lightmapped kind without the terrain layers.** Drives with it raised
  the lit interiors' `Tiled.Opaque` by about 0.1 ms: more ranges and
  pipeline changes, with no occupancy gain. Rejected.
- **Restructuring the volume lookup** (one dot product per probe instead of
  four running SH rows): 0.04 ms of the mean, inside the p95 noise. Not
  shipped.
- **A cutout depth pass, terrain LOD, scatter proxy cells.** Measured on the
  same segment with no gain: its trees are opaque, terrain geometry did not
  change the vertex phase, and proxies cut 24% of its draws without
  changing frame time.
- **Splitting the opaque pass by the data it reads**, PETAFAB's rule for
  keeping each pass's working set in the L1 cache. On 2026-10-29 a 2 s Metal
  counter trace of the Bistro street view (2560×1440, MetalFX at 0.6, M1 Pro,
  shipped kinds) over `Tiled.Opaque` read fragment occupancy 17%, ALU
  limiter 44%, texture cache limiter 3%, last-level cache limiter 10%,
  texture sample limiter 10% and read bandwidth 6%: still latency-bound at
  low occupancy, not cache-bound, so a split would add passes without a
  limiter to relieve. The desktop `Lighting.Deferred` shows the same pattern
  (ADR-087). Not pursued; the register work that followed is
  [ADR-138](138-tiled-opaque-register-budget.md).

## Revisit when

Another material path (decals, rectangle lights, a heavy Custom graph)
shows the same compile-out gain, or the general pipelines' Standard draws
become rare enough that a kind for lightmapped draws pays for its ranges.

## Evidence

Local, dirty-tree, non-authoritative; Apple M1 Pro, Release, validation
variables unset, 2026-10-20. Off is the same binary with
`VKR_STANDARD_KINDS_DISABLED=1`.

- Level Design Test: headless editor drives of `rec-20261019-231623`
  (`rec.drive` with `follow`) at 1146×668, MetalFX, Epic, dynamic
  resolution off, alternating, two runs each, with the shipped build:
  `Tiled.Opaque` p95 6.44, 6.54 → 5.76, 5.82 ms; mean 2.21, 2.24 → 2.18,
  2.18 ms; freight-line segment (80 to 97 s) mean 5.25, 5.29 → 4.91,
  4.81 ms; `gpu.submission` p95 12.23, 12.39 → 11.65, 11.71 ms, mean 8.14,
  8.27 → 8.21, 8.25 ms (unchanged within noise). An earlier pair of runs
  with a lightmapped third kind raised the mean of interiors by about
  0.1 ms (Alternatives).
- Bistro image: `vkr_harness snapshot` of a local MetalFX orbit case at
  frame 600 (`local-windowed`), kinds on against off: PSNR 73.07 dB, largest
  difference 7/255, inside the run-to-run MetalFX difference (74.15 dB,
  13/255, [ADR-128](128-tiled-two-phase-occlusion.md)).
- Unavailable: authoritative clean-tree profiles; Vulkan, which this change
  does not run.
