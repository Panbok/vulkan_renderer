---
status: proposed
updated: 2026-09-05
authority: proposal
---

# Terrain rendering

VKR has no terrain representation, heightmap loader, terrain material, or
terrain-specific renderer path. The legacy design's chunk and LOD types were
never integrated.

## Current implementation baseline

The renderer consumes cooked static geometry through
[vkr_resources.h](../../lib/src/renderer/resources/vkr_resources.h), creates
and owns mesh instances in
[vkr_mesh_manager.c](../../lib/src/renderer/systems/vkr_mesh_manager.c), and
syncs scene renderables in
[vkr_scene_system.c](../../lib/src/renderer/systems/vkr_scene_system.c). The
existing GPU-driven topology classifies candidate rows and emits indirect work;
it has no terrain exception.

## Proposed gap

Define a terrain vertical slice that can use the existing candidate and material
paths before proposing a separate terrain renderer. The slice needs one source
format, bounded tile residency, deterministic tile-to-geometry conversion, and
an explicit rule for how tiles participate in opaque visibility and directional
shadows. It should establish seams and displacement behaviour before adding
geomorphing, editing, physics, or procedural generation.

## Unsettled decisions

- Whether terrain is imported and cooked as static mesh tiles or generated from
  a heightfield at load time.
- Who owns tile residency and eviction: the asset system, a scene component, or
  a terrain subsystem.
- Whether LOD is selected on the CPU before candidate publication or by an
  extension of the GPU candidate contract.
- How neighboring tiles guarantee crack-free boundaries and consistent shadow
  geometry.
- Whether material blending needs a new source contract or can use existing
  material rows and texture slots.
