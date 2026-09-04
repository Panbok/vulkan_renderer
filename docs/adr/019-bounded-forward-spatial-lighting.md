---
status: implemented
updated: 2026-09-05
authority: adr
---

# ADR-019: Bounded punctual lighting and local probes

## Status

Accepted.

## Context

A scene-wide light prefix drops lights unpredictably, and selecting one probe
per draw causes large meshes to inherit the wrong local environment.

## Decision

Use a stable table of up to 128 punctual lights and a conservative 384-cell
fragment-local bitmask grid. CPU scene synchronization owns membership; shaders
perform exact range and spot-cone rejection. Four-vector, 64-byte light rows
share position/range, direction/cone, color/intensity and type semantics across
Metal and Vulkan shading paths.

Pack up to 16 ready local IBL probes per frame. Compute their influence from
fragment position and AABB weights; keep the global environment as fallback.
Diffuse uses ADR-038 coefficients and specular uses prefiltered cubemaps.
Prepared scene metadata can override exact glTF light-definition ranges at the
cold import boundary; malformed or unmatched overrides fail preparation.

Only the directional term samples CSM. Punctual lights have no shadow maps and
probe bounds/ranges are not geometry visibility. The removed hard influence-AABB
experiment is not an occlusion mechanism. GTAO attenuates local indirect diffuse
only and does not establish arbitrary wall or furniture occlusion.

## Consequences

Lighting is bounded and independent of draw partitioning. Large radii or probe
bounds can leak illumination through geometry. Spatial membership alone cannot
fix that limitation.

## Alternatives considered

A first-N global list makes visibility depend on source order. Per-draw probe
selection fails on large meshes. Hard light influence boxes produced discontinuous
slabs without solving diffuse occlusion.

## Revisit when

Scene scale exceeds these capacities or geometric local-light visibility is
required and has an accepted resource/cost budget.

## Implementation

[`vkr_lighting_system.c`](../../lib/src/renderer/systems/vkr_lighting_system.c),
[`vkr_render_packet.h`](../../lib/src/renderer/vkr_render_packet.h),
[`vkr_gpu_abi.h`](../../lib/src/renderer/vkr_gpu_abi.h), and production world/deferred shaders.
