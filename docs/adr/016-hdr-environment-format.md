---
status: implemented
updated: 2026-09-05
authority: adr
---

# ADR-016: HDR source delivery and cubemap sampling

## Status

Accepted.

## Context

Environment delivery and runtime directional sampling have different storage
needs. LDR sky textures cannot preserve HDR lighting energy.

## Decision

Load equirectangular HDR sources and convert them to runtime cubemaps. Preserve
linear HDR radiance through environment conversion and GGX specular prefiltering.
Skybox and specular lighting sample cubemaps with their native backend resources.
Source texture dimensions and prefilter mip count remain owned by the prepared
resource and shader contract.

Diffuse response uses GPU-resident L2 spherical harmonics under ADR-038; there
is no baked diffuse irradiance cubemap in the current production path. Local
reflection probes share this split representation and use fragment-space bounds
under ADR-019. IBL preparation remains backend-owned explicitly synchronized
work outside complete graph resource declarations.

## Consequences

HDR delivery stays compact and runtime specular sampling stays directional.
Preparation and resource lifetime remain asynchronous GPU concerns. Cubemap
presence does not imply geometric visibility or diffuse wall occlusion.

## Alternatives considered

LDR-only environments lose radiance range. Direct equirectangular runtime
sampling complicates filtering at poles and seams. Diffuse cubemaps were
replaced independently by ADR-038.

## Revisit when

A new environment format or dynamic capture requirement changes preparation,
filtering or publication ownership.

## Implementation

[`vkr_skybox_system.c`](../../lib/src/renderer/systems/vkr_skybox_system.c),
[`vkr_vulkan_ibl.c`](../../lib/src/renderer/vulkan/vkr_vulkan_ibl.c), and
[`ibl/`](../../lib/src/renderer/shaders/metal/msl/ibl).
