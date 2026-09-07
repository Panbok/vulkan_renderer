---
status: implemented
updated: 2026-09-07
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

Directional lighting samples CSM. Point and spot lights opt into a separate
bounded depth-array pool with `casts_shadow`. The default budget is 16 faces at
1024 squared: a spot uses one perspective view and a point uses six in
+X, -X, +Y, -Y, +Z, -Z order. Stable scene-light order allocates complete light
groups; requests that do not fit remain unshadowed. The budget and map size can
be reduced through `VkrShadowConfig`. Shadowed lights require a finite positive
range, and shadowed spot outer half angles must be below 90 degrees.

Application preparation owns the fixed frame-local view payload. Each physical
target image owns its local depth array through the existing graph resource and
GPU-completion lifecycle. All active local views render each frame, including
alpha-tested casters. Local shadows do not use directional fit retention or SDSM.
Graph reads name only the rendered layers. GPU culling tables size storage from
the current camera, directional and local view count and retain grown capacity.

Receivers use nine comparison-PCF Poisson taps with a 1.5-texel radius. Normal
offset is two local shadow texels and receiver bias is one texel, converted using
the perspective footprint at receiver depth. Point taps reconstruct rays and
reproject into adjacent faces. Both deferred and transparent lighting consume
the same local visibility semantics. Probe bounds/ranges are not geometry visibility. The removed hard influence-AABB
experiment is not an occlusion mechanism. GTAO attenuates local indirect diffuse
only and does not establish arbitrary wall or furniture occlusion.

## Consequences

Lighting is bounded and independent of draw partitioning. Unshadowed lights and probe bounds can still leak illumination through geometry.
A full 16-face D32 pool uses 64 MiB per physical target image, before culling
and upload buffers. Selected local views redraw every frame; rendering cost
scales with lights and caster overlap and has no accepted timing claim yet.
Static local-shadow caching and automatic importance selection remain future work.

## Alternatives considered

A first-N global list makes visibility depend on source order. Per-draw probe
selection fails on large meshes. Hard light influence boxes produced discontinuous
slabs without solving diffuse occlusion.

## Revisit when

Scene scale exceeds these capacities or geometric local-light visibility is
required beyond the accepted local-shadow pool.

## Implementation

[`vkr_lighting_system.c`](../../lib/src/renderer/systems/vkr_lighting_system.c),
[`vkr_frame_input.h`](../../lib/src/renderer/vkr_frame_input.h),
[`vkr_gpu_abi.h`](../../lib/src/renderer/vkr_gpu_abi.h), and production world/deferred shaders.
