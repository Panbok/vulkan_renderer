---
status: proposed
updated: 2026-10-13
authority: proposal
---

# Large-level rendering

Four features for large levels that mix a wide outdoor view with an indoor
base: per-object draw distance, simpler distant trees and rocks, baked
reflections for water planes, and room-by-room visibility indoors. Per-object
draw distance ([ADR-114](../adr/114-per-object-draw-distance.md)), rooms
and portals ([ADR-115](../adr/115-rooms-and-portals.md)) and merged far
scatter cells ([ADR-116](../adr/116-scatter-proxy-cells.md)) are
implemented; sections 1, 2 and 4 keep only their remaining work. Each section gives the problem, the options, a recommendation,
the data and owners, the cost, both pipeline classes, open questions,
phases and the evidence needed to accept it.

This proposal builds on accepted decisions and does not change them:
GPU-driven visibility ([ADR-028](../adr/028-gpu-driven-deferred-visibility-buffer.md)),
the one-phase HZB ([ADR-032](../adr/032-two-phase-confirmed-visibility.md)),
retained cascades ([ADR-041](../adr/041-retained-cascaded-shadows.md)),
local shadows and probes ([ADR-019](../adr/019-bounded-forward-spatial-lighting.md)),
population ([ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md#population)),
geometry LOD ([ADR-085](../adr/085-gpu-geometry-lod-and-terrain-geomorphing.md)),
world partition ([ADR-086](../adr/086-world-partition.md)),
the pipeline classes ([ADR-087](../adr/087-gpu-class-graphics-pipelines.md)),
probe authoring ([ADR-103](../adr/103-probe-and-volume-authoring.md)) and
reversed depth ([ADR-109](../adr/109-reversed-camera-depth.md)). It extends
the [planar reflections](planar-reflections.md) proposal and the
[world partition](world-partition.md) proposal's "Richer proxies" item. The
untracked drafts [foliage](foliage.md) and [automatic mesh LOD](mesh-lod.md)
cover overlapping ground; the sections below say where.

## Motivating level and current baseline

The Testbed project's scene "Level Design Test" is the motivating level. It
is not a harness case: acceptance evidence uses Bistro, as `AGENTS.md`
requires. Its saved overlay (2026-10-11) holds 54,017 entities:

| Content | Count | Drawn as |
|---|---|---|
| Brushes | 6,973 (4,615 visual, 2,270 solid, 78 clip, 10 trigger) | One generated mesh per brush, one submesh per face material, up to 16 ([vkr_scene_brush.c](../../runtime/src/renderer/systems/vkr_scene_brush.c)); no merging across brushes |
| Brush faces | 44,396 | Child entities of their brushes; no draws of their own |
| Scatter rules | 8, asking for 16,384 copies | About 9,500 copies under the caps of 2,048 per rule and 12,288 per scene ([vkr_scene_population.h](../../runtime/src/renderer/systems/vkr_scene_population.h)); one mesh-manager instance and one candidate per copy |
| Terrains | 2 km at 2 m, and a 16 km `backdrop` at 16 m | Tiles with seven geomorphed levels (ADR-085) |
| Water | 241 `water` hulls of a lake and a river | Visual brushes with the opaque greybox `water` look (roughness 0.2); no water shader |
| Reflection probes | 0 | Every draw takes the global environment, the sky |
| Movers | 111 | Moving casters (ADR-108) |

The scatter meshes are small: fir 152 triangles, spruce 228, larch 172,
shrub 48 and boulder 320, each one primitive and one material. The indoor
base lies about 50 m under the playable terrain.

These code facts bound every section:

- **No culling before the GPU.** `vkr_scene_build_world_draws`
  ([vkr_scene_frame.c](../../runtime/src/renderer/systems/vkr_scene_frame.c))
  emits every visible, loaded opaque, cutout, transmission and shadow
  candidate unculled. It sets `SHADOW_CASTER` on every candidate, so no
  object can opt out of shadows.
- **Cull cost grows with candidates × views.** `Cull.Classify` and
  `Cull.Encode` run one thread per candidate and view
  ([gpu_draws.metal](../../renderer/src/shaders/metal/msl/world/gpu_draws.metal),
  [deferred.slang](../../renderer/src/shaders/vulkan/slang/world/deferred.slang)).
  The views are the camera, each cascade, each cascade's dynamic-caster
  view and each local shadow face drawn this frame.
- **Classify tested no distance** before ADR-114. The 20 km far plane
  (ADR-109) put the whole backdrop and every scatter copy in the camera
  frustum; the measurements below predate the draw distance.
- **The HZB works only for a still camera.** History is rejected when the
  view-projection changes at all (ADR-028; `CAMERA_CHANGED` in
  [vkr_metal_packet_graph.inc](../../renderer/src/metal/internal/vkr_metal_packet_graph.inc)
  and [vkr_vulkan_deferred.c](../../renderer/src/vulkan/vkr_vulkan_deferred.c)).
  It never applies to shadow views. A walk or a drive has no occlusion
  culling.
- **LOD selects by projected error.** Cooked static meshes carry up to six
  more levels, each about half of the previous one. The cooker stops below
  64 triangles or when a level keeps more than 85% (ADR-085), so the level's
  scatter meshes get at most one or two coarser levels.

### Measured cost

The user reports the bridge view at about 6.3 ms of GPU passes, mostly
`Tiled.Opaque` at about 4.4 ms, and frames of about 7.8 ms (headless editor,
M1 Pro, 2026-10-11, after the caster-bounds fix of ADR-041). These figures
are indicative. They are not harness reports.

An earlier attribution hid one group at a time and read the headless
editor's `stats.frame_ms_p95` at still cameras (M1 Pro, 2026-10-11, before
the caster-bounds fix). It is indicative only. A still camera keeps the HZB
active, so it understates the cost of a moving camera.

| View | All | Without backdrop | Without terrain | Without scatters |
|---|---:|---:|---:|---:|
| Valley from the east | 9.96 ms | 8.61 ms | 8.00 ms | 9.54 ms |
| Bridge deck | 10.41 ms | 9.12 ms | 7.92 ms | 9.99 ms |
| Terminal (indoor) | 8.40 ms | 8.40 ms | 8.41 ms | 8.93 ms |
| Atrium (indoor) | 7.36 ms | 8.01 ms | 7.22 ms | 7.35 ms |

Two results bound the expected gains. First, scatters cost about 0.4 ms of
p95 in the valley views, so draw distance and far trees can save at most that
much there today. Second, the terrain rows include the cascade 3 redraw of
about 2 ms per frame that the later fix removed. Phase 0 below re-measures the
split before any feature lands.

### Why world partition does not solve this

An earlier review (2026-10-11) found that world partition (ADR-086) does not
lower the valley view's cost. The terrain, the backdrop, the scatters and the
water stay resident in every cell near the camera. The base lies under the
valley, so 128 m cells on the ground plane cannot separate the base from the
valley above it. The level has no `world_partition` singleton.

## Phase 0: measurement and the Bistro fixture

The fixture exists:
[`bistro_tiled_large_level_local`](../../assets/scenes/fixtures/bistro_tiled_large_level_local.scene.json)
puts Bistro inside a 2,048 m terrain at 8 m spacing and a 16,384 m backdrop
at 64 m, both written by
[make_terrain.py](../../tests/fixtures/rendering/large_level/make_terrain.py),
four scatter rules of 2,048 copies each with a 250 m proxy distance (fir,
spruce, boulder, shrub; [large_level](../../tests/fixtures/rendering/large_level/)),
and a café room with door, window and south-wall portals. Its cases,
`tools/cases/local/large_level_bistro_{metal,vulkan}_{overview,glide,cafe_walk}.case.json`
and the Metal `overview`, `cafe_bar` and `cafe_west` capture cases, hold a
still hill view, a 12 s glide down over the town and a 12 s walk through the
café. `VKR_DRAW_DISTANCE=0`, `VKR_ROOMS=0` and `VKR_PROXIES=0` switch each
feature off in one build.

Remaining:

- A water plane in the square and its two eye heights (section 3). The
  desktop pipeline's planar water uses its own Bistro fixture,
  `bistro_planar_water_local` (owner decision, 2026-10-13); this fixture
  still has no water plane.
- A matched Release report for the overview; ADR-114 to ADR-116 record
  those of the glide and the café walk on Metal.

## 1. Per-object draw distance

Implemented: [ADR-114](../adr/114-per-object-draw-distance.md) records the
component, the kind defaults, the cull word in the candidate state, the
per-view tests and the editor and agent surfaces. Remaining:

- **Fade band.** A dithered fade must discard, so classify would move a
  candidate inside its band into the alpha-tested bucket of its state for
  that frame. Without temporal history the tiled pipeline shows the dither
  as stipple, so add it only if captures show pops above the smallest size.
- **Evidence.** Retained-cascade redraw counts on the glide matched against a
  build without the feature; a matched `gpu.submission` report on the
  overview; the Vulkan twins under validation on Windows.

## 2. Simpler distant trees and rocks

Implemented in part: [ADR-116](../adr/116-scatter-proxy-cells.md) merges
each 64 m cell of a scatter's copies into one mesh of their coarsest level
past the scatter's `proxy_distance`, while the copies keep casting shadows.
Remaining:

- **Impostor cards.** Trees whose coarsest level is still heavy want
  crossed quads (or octahedral impostors) textured from a per-mesh atlas
  that Bakery cooks: base colour with alpha and normal from fixed views,
  with mips that keep alpha coverage, and a cutout material per mesh. The
  merged cell would then take the cards in place of the coarse level. The
  [foliage](foliage.md) draft orders octahedral impostors last (phase 7).
- **Fade across the swap**, with section 1's fade band, if captures show
  the swap.
- **Cells on job workers** when a rule's build time shows in edits.
- **Evidence.** Resident memory against a build that builds no cells, on a
  scene with heavier scatter meshes; a capture series across the swap
  distance. ADR-116 records the glide's matched timing.

## 3. Baked reflections for water planes

### What cannot be baked

A mirror reflection depends on the eye position. A reflected point lies
where the ray from the eye, mirrored at the plane, meets the scene. When the
eye moves, every reflected point moves by a different amount, depending on
its distance. An exact planar reflection therefore cannot be baked for a
moving camera. A bake can store what the plane reflects from one position,
or content far enough away that its parallax is small. Everything else needs
a correction or a real-time pass.

### Problem

The level's water reflects the sky: it has no probes, and neither pipeline
has SSR (owner decisions, 2026-10-06 for the tiled pipeline and 2026-10-12 for
the desktop pipeline). The real-time planar pass of the
[planar reflections](planar-reflections.md) proposal renders the mirrored
scene. At the valley and bridge views that scene is the terrain, the backdrop
and the forests, which are geometry-bound, so the pass could cost a large
part of `Tiled.Opaque`. ADR-019's per-probe `box_projection` and the
lookup fixes (2026-10-11) removed the cube edges on the lake but left it with
the global environment.

### Options

| Option | Correct for | Wrong for | Runtime cost (tiled) | Bake and memory |
|---|---|---|---|---|
| A. Far field at infinity: a probe on the plane, `box_projection` off | Sky, clouds, backdrop mountains | Valley walls, bridge, trees near the shore: parallax grows as eye offset over distance | A probe lookup | One cube per plane |
| B. Plane probe with a proxy box: a probe on the plane, the box set to the surrounding walls, `box_projection` on, the reflector hidden in the capture | Static content near the box walls, from any eye position | Content far from the box walls; dynamic objects; the player | A probe lookup | One cube per plane |
| B2. Mirrored-camera probe: B captured at the mirror image, below the plane, of a typical eye position | Eyes near that position | Eyes far from it; dynamic objects | A probe lookup | One cube per plane |
| C. Fixed-view planar image: the mirrored view baked from one eye position | That eye position | Every other position | One texture sample | One 2D image per plane |
| D. Hybrid: B as the far field, plus a real-time near-field pass limited to a reflection distance and to dynamic objects | Near static and dynamic content, and the far field | Content between the near limit and the box walls | The planar pass over a small part of the scene | B's cube and the pass target |
| E. Real-time planar pass alone (planar reflections proposal) | Everything it draws | Content it leaves out to save cost | The full mirrored view | The pass target |

The desktop pipeline (Vulkan) lost SSR's near field over the probes on
2026-10-12 (ADR-055). Its water also takes planar reflections (owner decision,
2026-10-12), so the same options apply there.

### Recommendation

Adopt B now, then measure D.

- **B, baked plane probe.** The planar reflector component of the planar
  reflections proposal gains a `baked` mode. Bakery's `bake probe` captures a
  cube at the plane's centre, just above the surface, with the reflector's
  own draws and everything below the plane hidden. The probe's box is
  authored to the surrounding walls; for open water it is a large box with
  `box_projection` on, or off for a plane seen only from far away. A
  `capture_height` below the plane turns B into B2 for a plane seen mostly
  from one height, such as the lake from the bridge. The reflector's draws
  use this probe directly, not the first probe whose influence at the
  draw's centre reaches one half, so a smaller valley probe cannot win over
  the plane's own probe. The cost is the
  existing probe lookup.
- **D, near field.** The real-time pass draws only candidates within the
  plane's `reflection_distance` and dynamic casters, using section 1's test
  with the reflection view's own distance. Its alpha marks covered pixels;
  the surface takes the pass where covered and the plane probe elsewhere.
  Accept it only if its measured cost fits the budget at the bridge view.
- **Decline C.** It is correct from one position only.

| Mode | Shows | Visible error |
|---|---|---|
| `baked` (B) | Static content, box-corrected | Parallax off the box walls; no dynamic objects |
| `hybrid` (D) | B plus near and dynamic content | Seam at `reflection_distance`, hidden by roughness and ripples |
| `realtime` (E) | All drawn content | None from parallax; its own cost |

Water roughness and a later water material with normal maps blur the
reflection, so parallax errors in B show less on water than on a mirror. A
water material is outside this proposal.

### Data and ownership

| Part | Owner |
|---|---|
| Reflector component: mode, probe reference, `reflection_distance` | Scene types; the component of the planar reflections proposal |
| Capture filter (hide the reflector and the half-space below it) | Bakery `bake probe` and the `bake.scene.json` capture scene (ADR-103) |
| Per-draw probe binding from the reflector | The camera view's encode kernel, which already writes the probe in `state_flags` bits 27 to 31 (ADR-087, decision 8) |
| Near-field pass | The planar reflections proposal |

### Cost and budget

B costs one prefiltered cube per plane, as any probe does, and no pass. It
uses one of the 16 probe slots per frame. D's cost is the planar pass on its
near field; the planar reflections proposal holds its target and estimates.

### Both pipeline classes

The reflector binding and the capture filter are shared. The tiled pipeline
reads the bound probe through its existing probe variant; the owner decided
on 2026-10-13 that it ships B first. The desktop pipeline already renders
the mirrored view in real time on reflector pixels
([ADR-117](../adr/117-desktop-planar-reflections.md)) and falls back to its
blended probes elsewhere, so B there serves only as that fallback. D runs
on the tiled pipeline only, as the planar reflections proposal scopes it.

### Open questions

| Question | Recommendation | Tradeoff |
|---|---|---|
| Accept "baked" as a plane-bound probe with a proxy box | Yes | It is not a mirror; dynamic objects do not reflect |
| Capture point on the plane or at a mirrored eye height (B2) | On the plane by default; `capture_height` per plane | A mirrored height is exact near that eye and worse elsewhere |
| Real-time near field on the tiled budget | Measure after B ships | Clear near reflections against a pass at the widest views |
| A water material (normal maps, Fresnel, depth colour) | A separate proposal | The greybox look keeps errors visible |

### Phases

1. Implemented 2026-10-13 on the tiled pipeline
   ([ADR-117](../adr/117-desktop-planar-reflections.md), decision 6), in a
   narrower form than above: the reflector names its probe by index
   (`probe`) instead of a `baked` mode, the capture filter hides planar
   reflectors' draws in every reflection-probe capture but does not clip
   content below the plane, and the binding is per frame through the
   probe's owner object id. The fixture
   `bistro_planar_water_local` carries the plane probe; the desktop
   pipeline ignores the binding.
2. Near-field pass with `reflection_distance`, after the planar reflections
   proposal's pass exists.

### Evidence

- The fixture's water plane at two eye heights. The real-time pass, drawn in
  a diagnostic build for the full scene, is the reference image. Report the
  mean error of B and D against it inside the plane's pixels. Still open
  for B: only the desktop pipeline draws the reference.
- Captures that show no water reflecting itself and nothing from below the
  plane.
- Matched Release timing for D against B at the high eye position.
- One focused Metal API validation run; on Windows, the Vulkan twin for B
  under validation.

## 4. Room-by-room visibility indoors

Implemented: [ADR-115](../adr/115-rooms-and-portals.md) records the
`room` and `portal` components, the membership of static draws, the portal
walk, the camera and cascade bits, the light and probe gates, doors that
shut portals, `room.probes`, `room.derive` and the `rooms` lint check,
and `blockout.room` and `blockout.doorway` create rooms and portals.
Remaining: the Vulkan twin under validation on Windows.

## Order and dependencies

```text
Phase 0: fixture and attribution
  |
  v
1. Draw distance (classify test)
  |            |              |
  v            v              v
2. Scatter  4. Rooms      3. Plane probe (B)
   cells       |              |
               v              v
          room probes     3. Near field (D),
                          after the planar pass
```

Sections 1, 2 and 4 share the cull word in the candidate state and the
classify test (ADR-114 to ADR-116). Section 3's baked probe needs only the
reflector component and can ship early.

## Native checks per pipeline class

| Section | Tiled pipeline (Metal, M1 Pro) | Desktop pipeline (Vulkan, Windows) |
|---|---|---|
| 1 | Captures, timing, one Metal API validation run | Captures, timing, validation with synchronization checks |
| 2 | Captures, timing, memory | Captures, timing |
| 3 | Captures against the reference, timing | Probe binding captures |
| 4 | Captures on and off, timing on the moving walk | Same, with validation |

A build, a CPU test or a capture on one backend does not establish the
other pipeline's behaviour. Vulkan checks stay unavailable on the Mac and
are recorded as unavailable until a Windows host runs them.
