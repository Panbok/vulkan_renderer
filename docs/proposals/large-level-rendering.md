---
status: proposed
updated: 2026-10-13
authority: proposal
---

# Large-level rendering

Four features for large levels that mix a wide outdoor view with an indoor
base: per-object draw distance, simpler distant trees and rocks, baked
reflections for water planes, and room-by-room visibility indoors. Per-object
draw distance is implemented ([ADR-114](../adr/114-per-object-draw-distance.md))
and section 1 keeps only its remaining work. Each section gives the problem, the options, a recommendation,
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
and four scatter rules of 2,048 copies each (fir, spruce, boulder, shrub;
[large_level](../../tests/fixtures/rendering/large_level/)). Its cases,
`tools/cases/local/large_level_bistro_{metal,vulkan}_{overview,glide}.case.json`
and `large_level_bistro_metal_overview_capture.case.json`, hold a still hill
view and a 12 s glide down over the town.

Remaining:

- A water plane in the square and its two eye heights (section 3).
- Rooms and portals in the café interior and the café walk (section 4).
- Matched before/after Release reports per section; no report has yet
  isolated `Cull.*` for a still and a moving camera on matched builds.

## 1. Per-object draw distance

Implemented: [ADR-114](../adr/114-per-object-draw-distance.md) records the
component, the kind defaults, the cull word in the candidate state, the
per-view tests and the editor and agent surfaces. Remaining:

- **Fade band.** A dithered fade must discard, so classify would move a
  candidate inside its band into the alpha-tested bucket of its state for
  that frame. Without temporal history the tiled pipeline shows the dither
  as stipple, so add it only if captures show pops above the smallest size.
- **`view.cull_report`** and per-reason counts in `stats`, computed on the
  CPU from the host mirror of the camera test (the GPU compaction state keeps
  its size), and a `Draw distance` inspection mode.
- **Evidence.** Retained-cascade redraw counts on the glide matched against a
  build without the feature; matched Release `local-windowed-gpu-single` and
  `gpu.submission` reports on the overview and glide; one focused Metal API
  validation run; the Vulkan twins under validation on Windows.

## 2. Simpler distant trees and rocks

### Problem

A far forest costs one candidate, one instance row and one draw per copy,
whatever its screen size. The level's tree meshes are already small (48 to
320 triangles), so ADR-085's cooked levels stop after one or two steps. Their
cost at range is per copy: classification in every view, encoding, and many
sub-pixel triangles. An 11 m fir at 1 km covers about 14 rows at 1440 rows and
a 60-degree field of view, yet draws all of its levels' triangles. Production
trees with thousands of alpha-tested triangles will add raster cost that
cooked levels reduce only to their silhouette.

The [mesh LOD](mesh-lod.md) draft predates the cooked levels that ADR-085
records as implemented; its remaining items are thresholds and joint
simplification. The [foliage](foliage.md) draft lists a far representation
as gap G9 and orders octahedral impostors last (phase 7).

### Options

| Option | Fits | Cost and risk |
|---|---|---|
| A. Cooked LOD chain (ADR-085) | Heavy meshes | Exists. Gains little on small meshes; keeps one candidate per copy |
| B. Per-copy impostor as a last level | Hero trees seen at middle range | A different geometry and material per level, which the LOD table cannot express: it switches index ranges of one vertex set. Needs a vertex path that faces the camera and an atlas sampler on both backends. Keeps one candidate per copy |
| C. Cell proxies of scatters | Far forests and rock fields | One candidate per cell and material replaces hundreds. A swap per cell at a distance; the swap pops unless faded |
| D. Dense instancing tier (foliage draft D1) | Grass and undergrowth | Expands copies on the GPU; still one classify thread per copy and view |

### Recommendation

1. **Draw distance first.** Section 1 removes shrubs and boulders by screen
   size. It needs no new asset.
2. **Cell proxies for far scatters (C).** Each scatter rule divides its
   copies into 64 m cells. Past `proxy_distance` a cell draws one generated
   mesh per material in place of its copies:
   - Trees: two crossed quads per copy, placed by the copy's transform,
     textured from a per-mesh impostor atlas.
   - Rocks: the coarsest cooked level of each copy, merged and transformed.
   The runtime builds the cell meshes on job workers when the rule rebuilds,
   as brushes build generated meshes. They need no bake round trip and
   follow every edit.
3. **Per-copy octahedral impostors (B) later**, when a scene has trees whose
   middle range costs raster time. This is the foliage draft's phase 7.

`proxy_distance` must lie past the preset's `max_shadow_distance` (200 m on
High) and the range of every shadow-casting local light. The swap is a static
change listed with the cell's box (ADR-041, ADR-019):

- A retained cascade or cached face whose volume that box misses stays
  valid.
- With a low sun, a far cell can lie inside a cascade's volume along the
  light. That cascade then redraws once.
- The cell mesh stays a shadow caster whose bounds enclose its copies, so
  the measured caster box and its generation do not change.

The CPU swaps a cell with hysteresis (in at `proxy_distance`, out at 90% of
it) and at most 16 cells per frame.

**Transition.** At 200 m and more a tree spans few pixels, and the crossed
quads keep its silhouette and colour. The swap uses section 1's fade band
when `fade` is on; otherwise it pops.

**Bakery's role.** Bakery cooks one impostor atlas per tree mesh: base
colour with alpha, and normal, from a fixed set of views, with mips that
keep alpha coverage. The atlas is a cooked texture beside the `.vkb`, keyed
by the mesh's content hash. Bakery does not bake cell meshes, because copies
come from runtime placement and change with every terrain edit.

### Data and ownership

| Part | Owner |
|---|---|
| `proxy_distance`, cell size | Scatter rule (`SceneScatter`, [vkr_scene_system.h](../../runtime/src/renderer/systems/vkr_scene_system.h)) |
| Cell meshes, their lifetime and swap state | Population ([vkr_scene_population.c](../../runtime/src/renderer/systems/vkr_scene_population.c)); released with the rule's copies |
| Impostor atlas | Bakery and the cooker ([vkr_bakery_bake.c](../../tools/bakery/vkr_bakery_bake.c)); a cooked texture with the asset's lifetime |
| Card material | A cutout material per tree mesh that samples the atlas |

Copies past `proxy_distance` stay in the population's CPU state but leave the
candidate stream. Their mesh-manager instances stay, so swapping back needs
no allocation.

### Cost and budget

A cell past `proxy_distance` draws one candidate per material in place of
its copies, and a cell without copies draws nothing. A stand of 100 trees in
one 64 m cell becomes one candidate. On a 2 km terrain a rule has at most
1,024 cells, so its far candidates never exceed its 2,048 copies, and dense
stands gain the most.

Crossed quads take 8 vertices and 4 triangles per copy: about 76,000
vertices for 9,500 copies, a few MiB at most. An atlas texture of 2048² BC7
with mips is about 5.3 MiB, and a tree mesh needs two (base colour and
normal).

Budget: per-copy candidates only inside `proxy_distance`, and far scatters
below 1,000 candidates per scene.

### Both pipeline classes

Cards are an ordinary cutout material and generated mesh, so both pipelines
draw them through existing buckets. On the tiled pipeline they pay the
alpha-tested cost the foliage draft records (G8). Copies do not have
lightmaps; cards take the baked diffuse volume or the environment, as copies
do.

### Open questions

| Question | Recommendation | Tradeoff |
|---|---|---|
| Cell meshes built at runtime or baked | Runtime, with Bakery baking only the atlas | A bake fits large worlds better but goes stale at every terrain edit |
| Crossed quads or octahedral impostors for cells | Crossed quads | Octahedral impostors look right from every angle but need a new vertex path and a view blend on both backends |
| Cell size | 64 m | Smaller cells swap closer with smaller pops but add candidates |
| Order against the foliage draft's dense tier (D1) | Independent; cells apply to both paths | None |

### Phases

1. Section 1's fields on scatter rules.
2. Atlas cook for tree meshes; card material.
3. Runtime cell meshes and the swap.
4. Per-copy octahedral impostors, after evidence on a scene with heavy trees.

### Evidence

- On the fixture's outdoor glide: candidate count per view falls by the
  copies past `proxy_distance`. Cascade redraw counts rise by at most one
  per swapped cell at a low sun and are unchanged at a high sun.
- Captures at the swap distance: the mean absolute change across the swap
  frame stays below the change between two frames of the glide elsewhere.
- Matched Release timing as in section 1; resident memory of cells and
  atlases from the harness memory rows.
- A CPU test that fails when a cell mesh holds a copy outside its cell or
  misses a copy inside it.

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
reads the bound probe through its existing probe variant; the desktop
pipeline blends probes per pixel, so the binding there raises the plane
probe's weight on the reflector's pixels to one. D runs on the tiled
pipeline only, as the planar reflections proposal scopes it.

### Open questions

| Question | Recommendation | Tradeoff |
|---|---|---|
| Accept "baked" as a plane-bound probe with a proxy box | Yes | It is not a mirror; dynamic objects do not reflect |
| Capture point on the plane or at a mirrored eye height (B2) | On the plane by default; `capture_height` per plane | A mirrored height is exact near that eye and worse elsewhere |
| Real-time near field on the tiled budget | Measure after B ships | Clear near reflections against a pass at the widest views |
| A water material (normal maps, Fresnel, depth colour) | A separate proposal | The greybox look keeps errors visible |

### Phases

1. Reflector `baked` mode, capture filter and per-draw binding.
2. Near-field pass with `reflection_distance`, after the planar reflections
   proposal's pass exists.

### Evidence

- The fixture's water plane at two eye heights. The real-time pass, drawn in
  a diagnostic build for the full scene, is the reference image. Report the
  mean error of B and D against it inside the plane's pixels.
- Captures that show no water reflecting itself and nothing from below the
  plane.
- Matched Release timing for D against B at the high eye position.
- One focused Metal API validation run; on Windows, the Vulkan twin for B
  under validation.

## 4. Room-by-room visibility indoors

### Problem

Inside the base the camera sees one to three rooms, but every candidate of
the level is classified in every view, and those inside the frustum are
encoded and drawn unless the HZB rejects them. The HZB rejects nothing while
the camera moves (ADR-028, ADR-032). The terrain, backdrop, scatters and
water above the base all pass the frustum test when the camera looks up or
along a corridor. Lights and moving-caster shadow faces in rooms behind
walls stay in the frame. With no probes, indoor surfaces reflect the sky.

### Options

| Option | Works while moving | Gates lights and shadows | Authoring | Cost and risk |
|---|---|---|---|---|
| A. HZB only | No | No | None | Exists. Two-phase occlusion was declined for Bistro (ADR-032); an indoor scene might have the deferred set it lacked, but it adds a second classify, raster and pyramid |
| B. Rooms and portals traversed each frame | Yes | Yes | Room volumes and portal quads | CPU traversal of tens of rooms per frame; a missing portal hides content |
| C. Precomputed PVS per cell | Yes | Yes | Cells; a bake | Every brush edit invalidates the bake; doors must be assumed open; memory grows with cells squared |
| D. Rooms derived from brushes | Yes | Yes | A flood fill of empty space | Leaks through gaps; poor fit for open halls; useful as an editor aid |

### Recommendation

Choose B, with D as an editor aid that proposes rooms and portals.

**Authoring.**
- A `room` component: its entity's transform places a box, as
  `look_volume` and `terrain_cutout` do. A room may hold several boxes as
  children for L-shaped spaces. A `sealed` flag marks a room no sunlight
  reaches.
- A `portal` component: its transform places a rectangle that touches two
  rooms or a room and the outside. An optional mover reference closes it
  while that mover rests at its closed pose.
- `blockout.room` and `blockout.doorway` create the room and the portal with
  the walls, because they already know the interior box and the opening.
- `room.derive` floods the empty space of a region on a coarse grid and
  proposes rooms and portals in the Agent changes window.
- A `rooms` check in `level.lint` reports portals that do not touch two
  spaces, gaps in room walls larger than a tolerance, and drawn objects in
  an indoor region that no room holds.

**Traversal.** Each frame the CPU finds the camera's room, or the outside,
and walks open portals that pass the frustum, narrowing it to each portal's
screen rectangle. The result is a bitset of visible rooms and a flag for the
outside.

**Membership.** At publication each object gets up to two rooms that its
bounds touch within a 0.5 m margin, so a wall between two rooms belongs to
both. An object that touches more than two rooms, or none inside an indoor
region, is unassigned and tested by the frustum alone. Terrains, backdrops,
scatters and water belong to the outside.

**What rooms gate.**

| Work | Rule | Retained state |
|---|---|---|
| Camera draws | Classify drops a candidate whose rooms are all hidden | None |
| Cascades | An object whose rooms are all `sealed` skips every cascade; other objects are not gated | The `sealed` set changes only by edits, so retained layers stay valid |
| Local lights | A light whose room is hidden, and whose range sphere reaches no visible room, leaves the frame's light grid | Its cached faces stay; they redraw only when stale |
| Local faces | Dynamic squares only for lights that stay; static redraws of hidden lights wait | No change to the cache rules |
| Reflection probes | Each room may own a probe with its box; a draw in a room takes that room's probe; only probes of visible rooms fill the 16 slots | Probe bakes unchanged |

On the tiled pipeline static lamps are baked into the lightmap (ADR-108), so
light gating there saves dynamic lights and the moving-caster faces of baked
lamps. On the desktop pipeline it also removes unbaked lamps.

**Probes per room.** This is the per-room probe placement the level lacks.
`room.probes` creates one probe per room, with the room box as the probe box
and `box_projection` on, and Bakery captures them. Indoor surfaces then
reflect their room instead of the sky. A room-bound probe also stops an
outdoor probe from reaching indoors through the largest-influence rule.

### Data and ownership

| Part | Owner |
|---|---|
| `room` and `portal` components | Scene types |
| Room graph, traversal and the visible bitset | A new runtime module beside the scene frame; its graph rebuilds at edit time, its bitset per frame |
| Per-candidate rooms | Two 16-bit room indices in the section 1 side array's second word |
| Visible-room bitset | A per-frame buffer of up to 1,024 bits (128 bytes) beside the cull root |
| Light and probe gating | The light and probe packing on the CPU ([vkr_standard_scene_runtime.c](../../runtime/src/application/vkr_standard_scene_runtime.c)) |

### Cost and budget

The side array grows to 8 bytes per candidate, at most 2 MiB at capacity,
per frame slot. Traversal visits rooms and portals, not objects; at 1,024
rooms it stays far below 0.1 ms of CPU. Classify adds one bit test before the
frustum test. Indoors, the outside flag removes the terrain, backdrop,
scatters and water from the camera view whenever no outside portal is
visible.

### Both pipeline classes

Traversal, membership, light and probe gating are CPU work shared by both.
The bit test is a shared kernel in both classify kernels. On the desktop
pipeline, hidden lights also leave the light mask and transmission work.

### Open questions

| Question | Recommendation | Tradeoff |
|---|---|---|
| Explicit rooms or rooms derived from brushes | Explicit, created by the blockout operations, with `room.derive` as an aid | Derived rooms need no authoring but leak through gaps and fit open halls poorly |
| Rooms per object | Two, else unassigned | More rooms per object cost bits per candidate |
| `sealed` rooms skip cascades | Yes, as an authored flag | A wrong flag removes real sun shadows |
| Rooms per scene | 1,024 | A larger base needs more bits per frame |
| Two-phase occlusion for indoor scenes (reopening ADR-032) | Not now; revisit if rooms leave a large deferred set | Exact occlusion while moving, against a second raster |

### Phases

1. Components, membership, traversal and the camera gate; `blockout.room`
   and `blockout.doorway` emit rooms and portals.
2. Light, local face and probe gating; `room.probes`.
3. `sealed` rooms in cascades.
4. `room.derive` and the `rooms` lint check.

### Evidence

- A CPU test with an independent oracle: for a synthetic room graph and a
  camera, cast rays through the portals' openings; the traversal must list
  every room a ray reaches. It fails when a portal on the path is skipped.
- On the fixture's café walk, captures with rooms on and off must match
  within the default snapshot gate: rooms hide only what is not visible.
- Matched Release timing on the moving café walk, where the HZB is off.
  Report visible and drawn counts per view, `Cull.*`, `Tiled.Opaque` and
  the local shadow passes.
- On Windows, the Vulkan twin of the walk under validation with
  synchronization checks, and its timing with the same counts.

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

Sections 1 and 4 share the per-candidate side array and the classify
change, so they share one ABI revision. Section 3's baked probe needs only
the reflector component and can ship early.

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
