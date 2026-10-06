---
status: proposed
updated: 2026-10-06
authority: proposal
---

# Planar reflections on the tiled pipeline

Mirror-like surfaces on the tiled pipeline
([ADR-087](../adr/087-gpu-class-graphics-pipelines.md)) reflect clearly by
rendering the scene mirrored about each surface's plane. This proposal records
the technique, how it fits the tiled pipeline on Apple M-series, the open
choices and the evidence needed to accept it. No part of it is implemented.

## Problem and scope

The tiled pipeline reflects only the global environment today; glass reflects
the sky where the desktop pipeline reflects the street (ADR-087, Glass
evidence). Queued per-draw reflection probes
([Tiled graphics pipeline](tiled-pipeline.md#remaining-work)) serve glossy
surfaces. Planar reflections serve the few surfaces that must reflect clearly:
mirrors, still water, polished floors and glass panes viewed at grazing angles.

- **SSR is not planned for the tiled pipeline** (owner decision, 2026-10-06).
  It needs the frame's depth and colour before shading, so it would split the
  one on-chip `Tiled.Opaque` pass and store its attachments. It also needs a
  history to hide noise, which the tiled pipeline does not keep.
- **Probes alone fail for mirrors.** A cube captured at one point has
  parallax errors away from that point, even when box-projected, and it does
  not show dynamic objects or the player.

## Technique

1. Before `Tiled.Opaque`, render the scene from the camera reflected about the
   active plane into a reflection colour image.
2. Clip geometry behind the plane with an oblique near plane: the reflected
   projection's near plane becomes the mirror plane, so no extra clip
   distance is needed. The oblique projection costs depth precision far from
   the plane.
3. The reflective surface samples the image at its own screen position. The
   reflected camera has the main camera's projection, so the reflected point
   lands at the fragment's screen UV. A normal map may offset the UV, and
   roughness may select a blurred mip.
4. Outside the image, when no plane is active, or when the plane is disabled,
   the surface falls back to probes or the global environment.

The reflected view flips triangle winding, so the pass inverts its cull mode.
The raster buckets that already separate mirrored instances
([ADR-044](../adr/044-shader-cross-backend-contract.md), reflection parity)
keep working: the pass inverts the winding of every bucket.

## Tile-based GPU specifics

- **Own render pass.** The reflection pass is a separate render pass. Its
  colour must be stored, because `Tiled.Opaque` samples it; its depth stays
  memoryless. The graph declares the colour image with a viewport extent and
  a divisor, as `cloud_history` and `bloom_chain` do in
  [`tiled.rendergraph.json`](../../assets/render_graphs/tiled.rendergraph.json),
  under a new condition such as `planar_reflection_active` in the condition
  table of [`vkr_rg_json.c`](../../renderer/src/vkr_rg_json.c).
- **Reduced resolution.** Half or quarter resolution, one sample, no
  multisampled resolve.
- **Cheaper shading.** A forward variant without dynamic local lights or local
  shadows: lightmaps, baked diffuse volumes, the sun with the existing
  cascades, and the environment. [`tiled.metal`](../../renderer/src/shaders/metal/msl/world/tiled.metal)
  already selects one fragment variant per frame by its dynamic lights
  (`VkrMetalTiledLighting`); the reflection pass would use its variant
  without lights.
- **Culling.** The reflected view is one more view in the GPU-driven
  classify and encode kernels
  ([`gpu_draws.metal`](../../renderer/src/shaders/metal/msl/world/gpu_draws.metal)),
  next to the camera, cascade and local-shadow-face views. Its frustum
  planes include the mirror plane, so draws behind the plane are culled before
  encoding. The camera's HZB does not apply to it, as for shadow views.
- **Coarser LOD.** `vkr_gpu_lod_view` in
  [`vkr_gpu_abi.h`](../../renderer/src/vkr_gpu_abi.h) scales the LOD error by
  the view's image rows, so a half-resolution view already selects coarser
  levels ([ADR-085](../adr/085-gpu-geometry-lod-and-terrain-geomorphing.md));
  a per-view bias can coarsen them further.
- **Budgeted planes.** One active plane per frame, at most two, chosen on the
  CPU by screen coverage and distance. Others use probes.
- **Update rate.** A distant or small plane may update every second frame; a
  mirror near the camera updates every frame.
- **Shadows.** The pass reuses the camera's cascades and adds no shadow
  views. Reflected surfaces outside the cascade fit fall back to the last
  cascade or to no sun shadow.

Every cost here is an estimate to be measured. Vertex work does not shrink
with resolution, and the widest Bistro views are already geometry-bound
([meshlet cluster culling](meshlet-cluster-culling.md)), so a reflected view
that sees most of the scene can cost a large part of the depth pre-pass even
at quarter resolution.

## Authoring and data

A scene component marks an entity as a planar reflector. The plane comes from
the entity transform: its origin and up axis. The component holds a
resolution tier (half or quarter), a maximum distance beyond which the plane
falls back to probes, and an enable flag. The editor shows the component in
Details and outlines the plane when the entity is selected. The reflector's
own draws sample the reflection image; other draws are unchanged.

## Open choices

Each choice has a recommendation; the owner decides.

- **Component or material flag.** Recommend a component: the plane is
  explicit, CPU selection is simple, and any material can reflect. A material
  flag would need a plane extracted per mesh and cannot choose between the
  planes of one mesh.
- **Resolution tier.** Recommend half resolution by default and quarter for
  water and large floors, both following the adaptive render scale (ADR-087,
  decision 12).
- **Lights and effects in the reflected pass.** Recommend lightmaps, baked
  volumes, the sun with the reused cascades, the environment and the sky;
  no dynamic local lights, local shadows, glass or atmosphere pass at first.
- **One or two planes per frame.** Recommend one, the largest on screen; allow
  a second only if measurement leaves headroom in the budget.
- **Glass and blended surfaces.** Recommend that the reflected pass draws no
  blended surfaces at first. A glass pane that is the active reflector samples
  the image in `Tiled.Blend`.
- **Reflector draws in `Tiled.Opaque`.** Recommend drawing reflector surfaces
  after the GPU-driven opaque draws in the same render pass, with their own
  fragment variant, so the reflection path costs other draws no registers
  (ADR-087, decision 11, measured that unused light code costs registers).

## Evidence needed to accept

- A tracked Bistro fixture scene with one added reflective plane, derived from
  [`bistro_tiled_local`](../../assets/scenes/fixtures/bistro_tiled_local.scene.json),
  and harness cases for its orbit and a capture view.
- Matched Release `gpu.submission` and pass timings (`Tiled.Opaque` and the
  new pass) at 2560×1440 on the M1 Pro, with and without the plane, against
  the 16.7 ms p95 budget, and with adaptive quality.
- Captures that show correct clipping at the plane, no geometry from behind
  it, and no leaks at the plane's edges or at screen borders.
- One focused Metal API validation run of the capture case.
