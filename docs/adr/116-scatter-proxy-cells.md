---
status: partial
updated: 2026-10-13
authority: adr
---

# ADR-116: Scatter proxy cells

## Status

Accepted (partial). Past a scatter's proxy distance, each 64 m cell of its
copies draws one merged mesh of their coarsest level in their place, on
both pipeline classes. Impostor cards for trees with heavy meshes, a fade
across the swap and building cells on job workers, from the
[large-level rendering](../proposals/large-level-rendering.md) proposal, are
not built. The tiled pipeline has native Metal evidence; the desktop
pipeline has compiled SPIR-V only.

## Context

A far forest cost one candidate, one instance row and one draw per copy,
whatever its screen size. Small scatter meshes keep one or two cooked
levels ([ADR-085](085-gpu-geometry-lod-and-terrain-geomorphing.md)), so at
range their cost is per copy: classification in every view, an indirect
command and a draw each. The proposal suggested crossed-quad cards from a
Bakery atlas; the runtime has no image writer, and a swap that changes the
static candidate set redraws retained shadows
([ADR-041](041-retained-cascaded-shadows.md)).

## Decision

1. **Authoring.** `SceneScatter.proxy_distance` (metres, 0 off;
   [vkr_scene_system.h](../../runtime/src/renderer/systems/vkr_scene_system.h))
   turns proxies on for a scatter. Keep it past the preset's shadow distance
   (200 m on High).
2. **Cells.** When population builds a rule
   ([vkr_scene_population.c](../../runtime/src/renderer/systems/vkr_scene_population.c)),
   it sorts the copies into cells of 64 m on the ground plane, at most 1,024,
   and builds per cell one mesh-manager mesh with one submesh per mesh
   submesh: the copies' coarsest level, read from the cooked file through
   the raycast cache's held decode (`vkr_mesh_raycast_coarse_ranges`), moved
   to world space and sharing the copies' materials. The meshes are static,
   picked as the rule's entity, cast no shadow and take the copies' draw
   distances. A rule whose mesh has not finished publishing builds its cells
   in a later update.
3. **Swap.** Each frame (`vkr_scene_population_mark_proxies`) a cell whose
   bounding sphere lies past the proxy distance from the camera turns far,
   and back within 90% of it. The emission sets the camera's hidden bit of
   [ADR-114](114-per-object-draw-distance.md) on every copy of a far cell and
   on the merged mesh of every near one. Copies stay candidates and keep
   casting shadows, so the static candidate set never changes and a swap
   redraws no retained shadow.
4. **Switches.** A probe capture, the editor Scene's "All" and the
   `VKR_PROXIES=0` diagnostic draw every copy and no merged mesh. The
   runtime marks the cells of the rendered scene and of every added scene
   into one frame table, since an unmarked cell would draw beside its
   copies.

## Consequences

- A far cell costs one candidate per submesh instead of one per copy and
  submesh in the camera view; its copies still pay classification in every
  view.
- The merged meshes take the copies' coarsest level, which the GPU LOD
  selection would choose at that range anyway, so the swap changes little
  on screen; it pops when the coarsest level differs visibly.
- Merged geometry costs memory per copy (the fixture's fir: its coarsest
  level for each copy of the cell).
- On the large-level fixture, whose copies are 48 to 320 triangles and
  already leave the camera below 2 px, proxies pay little: on the glide
  (`large_level_bistro_metal_glide`, `local-offscreen-gpu-single`, one
  non-authoritative run each, 2026-10-13) the camera's visible rows fell
  from a median of 4,490 to 4,134 while `Tiled.Opaque` stayed at 4.8 ms. The
  overview capture changed 0.45% of pixels, mostly sky through automatic
  exposure. Proxies are for scatters of heavier meshes.
- Matched Release timing on the glide (2026-10-13; M1 Pro, Metal 4, tiled
  pipeline, 1280x720 hidden window with immediate presentation;
  `large_level_bistro_metal_glide_windowed.case.json`,
  `performance-windowed-gpu.json`, five children each, authoritative with
  matching fingerprints, clean tree at `6752317b`), against `VKR_PROXIES=0`
  in the same binary: `frame.wall` 14.815 ms off and 14.827 ms on (child
  means' standard deviation 0.013 and 0.034 ms), no gain; `Tiled.Opaque`
  9.778 to 9.748 ms; the camera's visible rows a mean of 5,373 to 4,247;
  `cpu.world_payload_build` 1.29 to 1.06 ms. GPU memory was the same,
  since the diagnostic stops the swap but the cells stay built. Reports
  `sha256:95a0ddebcf4ceff870395c4187408c8ddb6be7c794e8ef717c5c36092aa024f1`
  (on) and
  `sha256:0bd6eea71fe76e32450bf184d932ae9dd9e0947ce8559b45384d42bfdd19a45b`
  (off). A first proxies-on report with one child slowed by another
  process's build was discarded.

## Alternatives considered

- **Crossed-quad cards from a Bakery atlas** (the proposal): deferred until a
  scene has trees whose coarsest level is still heavy; it needs an image
  writer, a cutout material per mesh and coverage-preserving mips.
- **Removing far copies from the candidates.** Rejected: a static change
  per swap would redraw retained cascades as the camera moves.

## Revisit when

Trees with thousands of triangles at their coarsest level reach a level, or
captures show the swap.
