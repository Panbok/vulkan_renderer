---
status: partial
updated: 2026-10-10
authority: adr
---

# ADR-108: Baked lamp direct pages

## Status

Accepted (partial). On the tiled pipeline (Metal), a lightmap set bakes each
lamp group's direct light on its own pages at a higher texel density, and
the forward shader samples them bicubically. Lamps with an emitter radius
bake soft shadows on both bakers. Moving casters shadow the two baked lamps
that light them most on the tiled pipeline as they do on the desktop
pipeline ([ADR-104](104-desktop-baked-lamps.md)). The Metal bake and runtime
ran on the M1 Pro. The Vulkan kernel change compiles only. Entities that
move at runtime (moving physics bodies, mover-driven brushes) cast as
moving objects and stay out of the bake; the level's doors, lifts, tram and
fans shadow its baked lamps at runtime.

## Context

On the tiled pipeline a scene with a lightmap set lights its static lamps
only through the lightmap ([ADR-087](087-gpu-class-graphics-pipelines.md)
decision 11). A lamp group's direct light and its bounce shared one plane at
8 texels per metre (12.5 cm per texel), smoothed and sampled bilinearly, so
the zero-radius lamps of the Testbed project's "Level Design Test" scene cast
blurred, stepped shadows at every Shadow quality (user report, 2026-10-10).
Stationary lamps ([ADR-107](107-stationary-lamps.md)) shaded the lamps at
runtime instead; the owner's review found ownership seams, pixelation on
Epic and a halved frame rate.

Owner decision (2026-10-10): bake every static lamp again, its shadows
included, at a higher density for direct light, with bicubic sampling and
lamp-size penumbrae; moving objects, such as the player and physics bodies,
take runtime shadows from the lamps that light them most.

## Decision

1. **Lamp-direct pages.** A tiled bake with `--direct-texels-per-unit`
   above zero (Bakery's default 16; the Bake settings window's "Lamp shadow
   density") packs the lightmapped instances that a lamp group's lamps may
   reach a second time at that density
   ([`vkr_lightmap_baker.cpp`](../../tools/vkr_lightmap_baker.cpp),
   `lamp_lit_instances`). Each lamp group with lights then gathers only its
   bounce, sky and emission on the regular pages, and its direct light on
   the lamp-direct pages through a direct-only gather: the texel direct
   term's 3×3 footprint grid and, for buried-texel detection, first hits
   only (`bake_direct_pages`, flag 64 of the gather kernels). Buried texels
   take their neighbours' light; nothing is smoothed. The pages encode as
   ASTC 4×4 HDR. Since 2026-10-10 every page layout, the regular pages'
   too, places each rectangle, tallest first, at the lowest, then leftmost,
   place on the first page whose skyline holds it
   (`vkr_bake_lightmap_pack` in
   [`vkr_bake_lightmap.cpp`](../../tools/bake/vkr_bake_lightmap.cpp)); a
   single open shelf had left the room beside each tall rectangle empty.
   Brush and blockout charts lie with their longer side along the atlas's
   x (`vkr_brush_lightmap_chart`); a long face standing on end had made a
   long brush's atlas square and nearly empty.
2. **Soft lamp shadows.** A point or spot lamp's `source_radius` reaches
   the bake. The texel direct term casts 8 shadow rays from each of its 9
   footprint points toward a golden-angle spiral of 72 points on the lamp's
   disc facing the receiver, turned once per texel: the k-th ray of point
   p takes spiral point p + 9k, so the footprint samples the disc
   stratified as a whole (before 2026-10-10, 4 rays per point at a random
   turn each, which left grain in wide penumbrae). A bounce path casts one
   ray toward a random disc point
   (`sphere_light_point` in
   [`vkr_bake_metal.mm`](../../tools/bake/vkr_bake_metal.mm) and
   [`vkr_bake_lightmap.slang`](../../tools/bake/vkr_bake_lightmap.slang)).
   The unshadowed light stays the point light's. A zero radius bakes as
   before, with the same random draws.
3. **VKLM v5 and v6** ([`vkr_lightmap_set.h`](../../runtime/src/assets/vkr_lightmap_set.h)):
   `VKR_LIGHTMAP_PLANE_LAMP_DIRECT` planes on lamp groups; the header names
   the lamp-direct page count, density and rectangle table (bytes 104 to
   119); each instance has a 20-byte rectangle on those pages, all zero for
   an instance no lamp reaches; the lamp-direct pages follow every other
   page in the payload. Version 6 is the same file with the turned brush
   and blockout charts; a version 4 or 5 set still decodes, but brushes and
   shapes do not bind it (`shape_charts_current`), since their UVs would
   map onto other texels, so they keep the volume's light until a rebake.
4. **Runtime.** The loader uploads the lamp-direct pages as a second 2D
   array, slice page × planes + plane, and the rectangles
   ([`scene_loader.c`](../../runtime/src/renderer/resources/loaders/scene_loader.c));
   the frame's lightmap binding names each active layer's plane. The tiled
   vertex stage maps the draw's lightmap UV into its lamp-direct rectangle,
   and the fragment adds the plane to its layer's irradiance through a cubic
   B-spline of four bilinear taps (`vkr_metal_tiled_lamp_direct` in
   [`tiled.metal`](../../renderer/src/shaders/metal/msl/world/tiled.metal)).
5. **Moving casters.** The lighting system's baked shadow-casting lamps
   reach the local shadow resolve on both pipeline classes, so the two that
   light the moving casters most take static and composite shadow views
   (ADR-104). Frames with such lamps take the tiled `CASTERS` variant (every
   dynamic light, as the ALL variant), which subtracts from a lightmapped
   surface's irradiance each lamp's light times its static view's
   visibility minus its composite view's, through the tiled tent filter
   (`vkr_metal_tiled_moving_shadow`); the inspection variant does too.
6. **What moves.** An entity moves at runtime when it or an ancestor
   within 64 levels carries an enabled kinematic or dynamic physics body,
   or, for a brush or blockout shape, a `mover`
   (`vkr_scene_entity_moves` in
   [`vkr_scene_system.c`](../../runtime/src/renderer/systems/vkr_scene_system.c);
   owner rule, 2026-10-10: no or static collision is static, dynamic
   physics is dynamic). Its meshes cast as moving objects: a brush or shape
   when its mesh publishes, an imported mesh when it attaches, and every
   mesh again after a body edit (`shadow_mobility_dirty`); a mesh renderer
   that stops moving keeps moving-caster mobility until it reloads. The
   bake leaves the same entities out (ADR-088), so no shadow is both baked
   and subtracted at runtime.

## Consequences

- Lamp shadows show no texel steps; their softness follows the lamp's
  radius and the lamp-direct density, not Shadow quality.
- A frame samples four more bilinear taps per active lamp group on
  lightmapped surfaces.
- The level's set grew from 101 MB to 319 MB with 13 lamp-direct pages of
  4096²; five of them each held one long transit brush's square, nearly
  empty atlas. The skyline packing and turned charts bring it to 151 MB:
  2 regular and 5 lamp-direct pages, 66.6 million lamp-direct rectangle
  texels instead of 132.1 million (same exported scene, 2026-10-10).
- The stratified spiral doubles the texel direct term's shadow rays: the
  level's lamp-direct gather took 58.1 s instead of 45.5 s.
- The editor saves this level in about 10 minutes after the lamp edits,
  which the bake's own `scene.save` does not need.
- In this level the first-person player body is hidden, so it casts no
  shadow. Its doors, lifts, tram and fans move, so they cast lamp shadows
  through the moving-caster views. Before 2026-10-10 they were static
  casters, out of the bake, and cast no lamp shadow on the tiled pipeline;
  making them moving casters then forced the sun cascades, which redrew
  whole while a moving caster was in them, to redraw every frame (lobby
  passes 2.2 to 5.4 ms). The cascades now keep a static layer and draw
  only the moving casters over a copy of it (ADR-041).

## Alternatives considered

- **Stationary lamps** ([ADR-107](107-stationary-lamps.md)): removed.
- **Baked distance-field shadows**: crisp edges at low density with a few
  runtime-lit lamps per surface; the owner chose the plain bake.
- **The whole lightmap at 16 texels per metre**: four times the indirect
  bake time and memory for the same direct result.

## Evidence and remaining checks

Release, M1 Pro, Metal, 2026-10-10; local and non-authoritative.

- **CPU.** `vulkan_renderer_tester --suite run_lightmap_bake_tests`
  (`test_lightmap_set_lamp_direct_round_trip_and_rejects`: lamp-direct
  pages after the other pages, rectangles and plane lookup survive a round
  trip; a lamp-direct plane without pages, pages without a plane, a plane on
  a sun key and a rectangle off its page are rejected; an all-zero rectangle
  is accepted), `run_scene_loader_tests`, `run_metal_packet_abi_tests`,
  `run_metal_material_tests`, `run_lighting_system_tests`,
  `run_shadow_system_tests` and `run_scene_edit_tests` pass in Debug.
- **Vulkan sources.** `slangc` compiles `lightmap_gather`, `probe_gather`
  and `lightmap_trace_benchmark`; none ran.
- **Bake.** The editor's `lighting.bake` of the Level Design Test scene
  (lamp `source_radius` 0.25, volume spacing 1.5): the lightmap job took
  about 181 s, of which the lamp-direct pages 43.6 s; set 318,982,400 bytes.
- **Captures.** The headless editor at Epic shows soft lamp shadows without
  texel steps in the lobby, at the reception chairs and in the cafeteria.
- **Moving casters.** A fan under the hall ceiling casts a soft runtime
  shadow of a baked lamp on the ceiling, which disappeared when its mover
  was removed (Edit mode, Epic). `run_scene_physics_tests`
  (`physics_test_moves`: a dynamic body's child moves, a static body's
  does not, a brush below a mover moves and a plain entity there does not;
  a body edit marks mobility dirty) and `run_lightmap_bake_tests`
  (`test_bake_scene_leaves_out_moving_brushes`: a brush below a dynamic
  body adds nothing to the bake, one below a static body bakes) pass.
- **Movers' cost.** Same scene (4,275 lightmapped instances, 76 mover
  brushes), headless editor at Epic, `VKR_RG_GPU_TIMING=1`, median of the
  last 40 frames, passes in ms for the lobby, cafeteria and fan views:
  movers as static casters 2.29, 2.48, 2.59; movers as moving casters with
  whole cascade redraws 5.61, 5.67, 5.77; with the cascades' static layers
  (ADR-041) 3.27, 3.35, 3.84. The remaining cost is `Cull.Encode` for the
  moving-caster views (0.5 to 0.7 ms) and the `CASTERS` variant (0.2 ms,
  0.45 ms with the fan in view). One build per row from the same tree;
  local and non-authoritative.
- **Metal API validation.** `MTL_DEBUG_LAYER=1`, the fan view and Play,
  about 3,570 frames: no validation message.
- **Timing.** The headless editor's Scene view at Epic with
  `VKR_RG_GPU_TIMING=1`, median of the last 40 frames, against the
  stationary build's frames with the stationary shading left out, which
  matched the baked frame (ADR-107): lobby passes 2.17 against 2.09 ms,
  `Tiled.Opaque` 1.20 against 1.11 ms; cafeteria passes 2.21 against
  2.05 ms, `Tiled.Opaque` 0.98 against 0.85 ms. Stationary lamps took
  3.57 and 2.80 ms. Not a matched Release A/B of the same commit; no
  timing here is authoritative.
- **Unavailable.** Metal shader validation aborts the editor on this level
  (a residency-set limit that predates this ADR); the Vulkan bake, the
  desktop runtime and the desktop cascade composition did not run.
