---
status: proposed
updated: 2026-10-09
authority: proposal
---
# Artist toolkit

The artist toolkit follows the level design toolkit
([ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md)). The level
toolkit designs and structures a level. The artist toolkit gives the
finished layout its look: materials, lighting, sky, fog, post-processing and
set dressing. Part 1 is implemented and recorded in ADR-084, the
Standard tier of Parts 2 and 5 in
[ADR-093](../adr/093-material-graphs-and-art-workbench.md), the bindings,
world size and lint of Part 6 in
[ADR-094](../adr/094-surface-themes-and-art-pass.md), layered Standard
materials in [ADR-095](../adr/095-layered-standard-materials.md), Custom
graphs on Metal in [ADR-096](../adr/096-custom-material-graphs.md), look
volumes in [ADR-097](../adr/097-look-volumes.md), the Environment panel
and presets in [ADR-098](../adr/098-environment-panel-and-presets.md), the
artist views and `query.luminance` on Metal in
[ADR-099](../adr/099-artist-views-and-luminance-queries.md), and the bake
settings, time scrubber, outlines and Lights window in
[ADR-100](../adr/100-lighting-tools.md), and the decal tool in
[ADR-101](../adr/101-decal-placement.md); this
proposal keeps their remaining scope and the other parts:

1. Surface tags and fixed greybox looks with measurement aids in the level
   toolkit.
2. A material graph editor in its own workbench tab.
3. Lowering rules that keep shader and pipeline counts bounded and known
   before the game runs.
4. The art pass that binds surface tags to materials.
5. Lighting, environment, look volumes and dressing tools.

Every action is a typed operation in the ADR-084 operation table, so the
viewport, the Cmd bar and agents share it.

## Settled decisions

The owner settled these on 2026-10-08.

| Decision | Choice | Rejected alternatives |
|---|---|---|
| Materials in the level toolkit | The level toolkit assigns surface tags. Each tag has a fixed greybox look that designers cannot change, with measurement aids | A palette of paintable dev materials, as in commit `83740c66` |
| Surface tags and greybox looks | One concept: the designer picks a tag, the tag shows its greybox look, and the art pass binds the tag to a material | Separate greybox colours and physical surface types |
| Material authoring | A node graph editor | Only a fixed parametric material with layers |
| Where artists work | A separate workbench tab ([ADR-089](../adr/089-editor-workbenches.md)) | Windows inside the Level Design workbench |
| Agent use | Agents create, edit, compile, preview and assign materials through operations, with batches, review and undo as in ADR-084 | Agents that edit material files on disk |
| Vertex offset (2026-10-09) | None in Custom graphs at first: it makes static shadow caches stale, changes culling bounds and needs motion vectors per draw. A later separate output with its own budget, once foliage needs it | A vertex-offset output now |
| Custom code node (2026-10-09) | None: raw shader text defeats the cost estimate, the tiled fallbacks and agent safety | A custom-code node |
| Layered Standard (2026-10-09) | A flag branch first, as terrain does; a layered variant axis, which doubles the 50 Metal shading states, only if Phase 3 measures a cost on draws without layers | A variant axis from the start |
| Undo for documents (2026-10-09) | The edit journal extends to asset documents, so review, rollback and agent-scoped undo apply to materials | Local snapshot undo, as in the Script editor |
| Over budget (2026-10-09) | The editor warns; packaging fails | Failing in the editor, which blocks experiments |
| Environment presets (2026-10-09) | Applied by copy, as component presets are; World inheritance shares values across levels | Presets by reference |
| Theme scope (2026-10-09) | The scene and the container; world-partition cells may add regions later | Per-region themes now |
| Desktop resolve of Custom graphs (2026-10-09) | Per-graph classification and indirect dispatch | One kernel that switches on the graph id and holds the largest graph's registers for every pixel |
| Shader cost (2026-10-09) | Register and instruction counts from the offline compilers where they exist, node estimates otherwise | Estimates only |
| Delivery (2026-10-09) | Phase by phase to `main`, each after its own evidence | One branch merged at the end |

## Goals and limits

The graph editor must give artists the flexibility of a node graph without
the permutation growth of Unreal Engine materials. These rules define that
limit. Each phase that adds shaders or pipelines must keep them.

1. **No shader compiles in a running game.** Bakery compiles every shader
   offline. The editor compiles only edited graphs, on the Bakery daemon,
   never on the UI or render thread.
2. **No pipeline is created after a scene or a world-partition cell reports
   ready.** The renderer creates a scene's pipelines on workers while the
   scene or cell loads.
3. **The pipeline count is known before packaging.** Bakery lists every
   pipeline of a project. A project budget bounds the graphs that add
   pipelines.
4. **Material instances never add shaders.** An instance changes parameters
   and textures only.
5. **One graph has one meaning on both pipelines.** The material model is
   part of the art-level contract
   ([ADR-087](../adr/087-gpu-class-graphics-pipelines.md), decision 3). When
   the tiled pipeline does not model an output, the graph shows the declared
   fallback for that output.
6. **The editor shows cost while the artist works.** Texture samples, layers,
   lowering tier and the pipeline change are visible on the graph.

## Current baseline

Facts at commit `0d408fe6`.

**Materials and shaders**

- A `.mt` file is `key=value` text parsed by
  [material_loader.c](../../runtime/src/renderer/resources/loaders/material_loader.c).
  It holds PBR factors, 14 texture slots and the clearcoat, sheen,
  anisotropy, subsurface, diffuse transmission and volume parameters.
  Unknown keys are ignored.
- The `shader=` and `pipeline=` keys do not select pipelines. The backends
  never read `shader=`; `pipeline=` only enters the shadow change key. 933
  assets carry them.
- No material has its own shader. Material features are flag bits in an
  immutable GPU material row, and fixed shaders branch on them. The tables
  hold 8,192 materials.
- **Metal (tiled).** Bakery builds one metallib. The renderer creates every
  pipeline at startup in `vkr_metal_packet_create_pipelines`
  ([setup](../../renderer/src/metal/internal/vkr_metal_packet_setup.inc)).
  The frame picks a shading variant by lighting (5), probe (2) and decal (2)
  state, never by material. That gives 20 opaque, 20 alpha-to-coverage and 10
  blend states: 50 shading pipeline states
  ([ADR-092](../adr/092-projected-decals.md), decision 5). The tiled
  pipeline does not model clearcoat, sheen, anisotropy or diffuse
  transmission.
- **Vulkan (desktop).** Geometry writes a visibility buffer. One compute
  kernel, `vk_gbuffer_resolve` in
  [deferred.slang](../../renderer/src/shaders/vulkan/slang/world/deferred.slang),
  evaluates every material with runtime branches on the row flags. Its four
  variants exist for capture and debug only. A `VkPipelineCache` persists
  pipelines; Metal uses an `MTL4Archive`.
- **Unused code costs registers on the tiled pipeline.** Splitting the light
  paths into variants lowered `Tiled.Opaque` in the dynamic Bistro case from
  12.99 to 10.89 ms median
  ([ADR-087, light shading variants](../adr/087-gpu-class-graphics-pipelines.md#light-shading-variants)).
  Any shader feature that every draw carries must show the same measurement.
- **Terrain is already a layered material.** It merges four `.mt` files into
  one row with flag 2048. The shared
  [terrain kernel](../../renderer/src/shaders/shared/terrain_kernel.slangh)
  blends the layers by vertex colour on both pipelines, with no extra
  pipelines.
- **Previews.** `vkr_bakery preview material` renders a sphere thumbnail in
  an offscreen harness run. No interactive preview exists.
- **Reload.** `vkr_material_loader_replace_live` swaps a material in place.
  No `.mt` editor or `.mt` writer exists.
- **Cost data.** Not present. No per-material texture-sample, instruction or
  register counts exist, and no cost view mode.

**Editor**

- **Workbenches.** General, Level Design, Terrain, Lighting and Scripting
  are built in; nine tabs fit
  ([editor_workbench.c](../../editor/src/editor_workbench.c)). The Lighting
  palette creates lights, sky, clouds, fog, volumetric fog and post process,
  switches three view modes and starts a bake.
- **Node graphs.** The Animation editor draws a pose graph of at most eight
  nodes with its own undo
  ([editor_animation.c](../../editor/src/editor_animation.c)). The UI library
  has no generic node widget.
- **Journal.** The edit journal covers scenes and containers only. The
  Script editor and the Animation editor keep local snapshot undo.
- **Content browser.** It lists Material assets with Bakery thumbnails. It
  cannot create a material.
- **Details.** Mesh and shape materials are read-only strings. A brush face
  has an editable material path. No material picker exists, and a material
  cannot be dropped onto an object.

**Brushes and measurement**

- Each brush face is a child entity with `char material[256]`, a path, and
  per-face UVs in metres per repeat, default 1 m
  ([vkr_scene_brush.h](../../runtime/src/renderer/systems/vkr_scene_brush.h)).
- The Level Design palette paints 16 dev materials from
  `assets/materials/dev/`. All use the grid texture with a tint and a
  roughness.
- Clip and trigger roles default to their dev materials, but a `material`
  argument overrides them.
- **Measurement aids.** The grid overlay, orthographic grid labels and level
  checks in metres exist. Not present: a measure tool, dimension labels, a
  player-scale reference and a texel density view.
- **Surface tags.** Not present.

**Environment and lighting**

- Every environment setting is a singleton component in scene JSON.
  This covers atmosphere, sun and moon, clouds, height fog, volumetric fog
  and `post_process` (exposure, white balance, contrast, saturation, bloom,
  DoF, motion blur, GTAO). A scene can inherit the World's instance.
  The display transform is AgX, and colour grading is analytic. No LUT
  exists.
- **Spatial overrides.** The only one is `fog_density_box`, which affects
  volumetric fog on the desktop pipeline only. No post-process, exposure or
  look volume exists.
- **Light authoring.** No light gizmos for range, cone or rectangle shape
  exist. No light-group manager exists, and no time-of-day scrubber.
  Reflection probes and diffuse volumes cannot be created in the editor.
- **Bake settings.** Bakery accepts `lightmap_settings` and
  `diffuse_settings`, but the editor never sends them.
- **View modes.** The editor offers Lit, Unlit, Detail lighting, Lighting
  only and Wireframe. The renderer enum also has normal and material-param
  modes on the desktop pipeline.
- **Decals and scatter.** `decal` places a projected base-colour decal.
  `scatter` and splines are level-toolkit population.

## Lessons from other engines

| Engine | Take | Leave |
|---|---|---|
| Unreal Engine 5 | Graph editor, material instances, material functions, layered blends; Substrate's view of a material as stacked slabs | Static switches and usage flags that multiply shaders. PSO precaching discovers pipelines at load and compiles them asynchronously, and delays or substitutes a draw when a pipeline is late |
| Unity | Shader Graph with typed ports and sub-graphs | Keywords whose combinations multiply variants and need stripping |
| Godot 4.4 | Pipelines compiled while assets load; a pipeline that covers all features draws while a specialized pipeline compiles in the background | Specialization compiled at runtime after first use |

The common failure is the same in each engine. Shader count grows with a
product of options, so the full set is unknown until the game runs. This
proposal makes the set a sum over graphs with a budget, computed by Bakery.

## Overview

The toolkit splits authoring between level design, art pass, material
authoring and rendering:

```text
Level Design workbench
  brush face: surface tag + mark
  greybox look (fixed, per tag)
        |
        v
Art workbench: surface bindings
  tag -> material instance (theme)
  face override (art-owned)
        |
        v
Material instance (.mt)
  graph + parameters + textures
        |
        v
Material graph (.mtg)
  lowered by Bakery to a tier:
  Instance | Standard | Custom
        |
        v
Renderer
  Standard: material row data
  Custom: per-graph pipelines,
          created at scene load
```

## Part 1: Surface tags and greybox looks

Implemented on 2026-10-09 and recorded in
[ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md#surface-tags-and-greybox-looks):
the tags none to emissive and the marks hazard, orange, blue, red, green and
dark on brush faces and blockout shapes, the fixed greybox looks per tag or
mark and face orientation with the 4 m metric grid, the art-owned face
`material`, the greybox view, `brush.set_surface`, `face.set_material`,
`query.measure`, `view.greybox`, the Level Design palette's Surface and
Mark rows, its measurement aids, and the migration of documents that name
retired dev materials. `level.lint`'s `too_narrow` and `low_ceiling` issues
and the Level checks window are the clearance hints.

Remaining here:

- **Project tags.** A project adds tags in its settings, each with one of
  the fixed greybox tones. They come with themes in Phase 3, which keep
  project data.
- **Physical uses of `surface=`.** Materials declare their tag
  (ADR-094), so imported meshes share the brushes' vocabulary; nothing reads
  it yet.
- **Physical uses.** Footstep sounds, impact effects and friction read the
  tag; no phase of this proposal schedules them.

## Part 2: Material graphs

Implemented on 2026-10-09 and recorded in ADR-093: graph documents, the
node set below except material functions, Standard lowering, instances,
graphs from definitions and the removal of `shader=` and `pipeline=`.
Remaining here: material functions (`.mtf`), the `decal` and
`terrain_layer` domains, the `surface=` and `world_size=` keys (Phase 3),
badges for outputs the tiled pipeline ignores, and costs from the offline
compilers.

### Documents

| File | Content | Owner |
|---|---|---|
| `.mtg` | A material graph: nodes, typed connections, exposed parameters, the output node and the material domain | Art workbench, agents |
| `.mtf` | A material function: a graph with inputs and outputs that other graphs use as one node | Art workbench, agents |
| `.mt` | A material instance: `graph=`, parameter values, texture assignments, `surface=` and `world_size=` | Art workbench, importers, agents |

A `.mt` without `graph=` uses the built-in Standard graph. Every existing
`.mt` therefore stays valid with its current meaning. Importers keep
writing `.mt` files. The migration removes the dead `shader=` and
`pipeline=` keys in the same change that adds `graph=`.

`world_size=` gives the material's real-world size per texture repeat in
metres. Brush faces that bind the material use it as their UV scale, so a
1 m brick texture covers 1 m without manual UV work.

The `.mtg` format is JSON. Node ids are stable strings chosen by the author
or the agent, so an agent can patch a graph without reading it again.

```json
{
  "version": 1,
  "domain": "surface",
  "alpha_mode": "opaque",
  "parameters": {
    "tint": {"type": "color", "default": [1, 1, 1]},
    "moss_amount": {"type": "float", "default": 0.3, "min": 0, "max": 1}
  },
  "nodes": [
    {"id": "brick", "type": "texture_set", "set": "Textures/brick_old"},
    {"id": "moss", "type": "texture_set", "set": "Textures/moss"},
    {"id": "mask", "type": "height_blend",
     "inputs": {"a": "brick", "b": "moss",
                "weight": "vertex_color.r", "amount": "$moss_amount"}},
    {"id": "out", "type": "surface_output",
     "inputs": {"base_color": "mask.base_color", "normal": "mask.normal",
                "roughness": "mask.roughness", "ao": "mask.ao"}}
  ]
}
```

### Domains

| Domain | Output | Pipelines it may add |
|---|---|---|
| `surface` | The full material model: base colour, metallic, roughness, normal, AO, emissive, opacity and the clearcoat, sheen, anisotropy, subsurface and transmission lobes | Standard: none. Custom: Part 3 |
| `decal` | Base colour and opacity. Normal, roughness and metallic follow the [decal channels proposal](decal-channels-and-receivers.md) | None in the first version: decals lower only to Standard |
| `terrain_layer` | One terrain layer | None: terrain layers lower only to Standard |

### Node set

| Category | Nodes |
|---|---|
| Inputs | Parameter (float, colour, vector, texture), texture sample, texture set (base colour, normal, ORM by naming convention), vertex colour, UV0, UV1, world position, world normal, object position, camera distance, time |
| Coordinates | Tile and offset, rotate, world-planar projection, triplanar projection |
| Math | Add, subtract, multiply, divide, lerp, clamp, saturate, one-minus, power, dot, remap, min, max, abs, swizzle, split, combine |
| Masks | Height blend, slope mask, world-height mask, vertex-colour channel, noise texture |
| Normals | Normal strength, blend normals, detail normal |
| Layers | Layer blend: up to four texture sets with weights |
| Functions | One node per `.mtf` function |
| Output | Surface output, decal output, terrain layer output |

The first version has no custom-code node and no vertex-offset output. Open
choices 1 and 2 give the reasons.

### Cost

Each node declares its texture samples and an ALU weight. The compiler sums
them per output, and the editor shows them on each node and on the output
node. The offline compiler's reports replace the estimates when they become
available (settled 2026-10-09). Each graph shows a cost line:

```text
Standard · 9 samples · 3 layers · +0 pipelines
Custom · 14 samples · +20 Metal, +1 Vulkan
```

An output that the tiled pipeline does not model shows a badge, for example
"sheen: ignored on Metal". The badge names the fallback that ADR-087's
art-level contract defines for that output.

## Part 3: Lowering tiers

Bakery's graph compiler gives every graph one of three tiers. The rules
decide whether a graph adds shaders. The editor shows the tier and the reason
for it.

| Tier | When | New shaders | Pipelines |
|---|---|---|---|
| Instance | A `.mt` that only sets parameters and textures of a graph | None | None |
| Standard | Every node is in the Standard set, the layer count is four or fewer, and the domain's outputs map to the material row | None: lowers to row data | None, or the layered variants the settled layering decision allows |
| Custom | Any node outside the Standard set, or more than four layers | One surface function per graph | Per graph: see the pipeline table below |

### Standard tier

The Standard set is every node whose result the fixed shaders can compute
from row data:

- texture sets with UV transforms;
- UV0, UV1, world-planar or triplanar coordinates;
- tint and scalar parameters;
- up to four layers blended by vertex colour, a mask texture channel, height,
  slope or world height (implemented in ADR-095 for vertex colour, mask
  texture, slope and world height; height blending by layer heights and
  per-layer UV transforms remain);
- one detail normal map;
- the material model outputs that exist today.

The compiler pattern-matches the graph to that shape and writes the row
fields. Most environment materials fit this tier: layered brick with moss,
painted metal with wear, tiled floors with grime.

Layering generalizes the terrain blend. The terrain kernel already blends
four texture sets on both pipelines behind flag 2048. The Standard tier moves
that blend from terrain-only weights to the mask sources above. The row gains
the layer UV sources and mask selectors in its cold segment.

### Custom tier

ADR-096 implements this tier on Metal. It generates MSL directly rather than
Slang, because the tiled forward shader is MSL. Vulkan draws each Custom
material's fallback, and the desktop design below remains.

Bakery generates a Slang surface function for a Custom graph. The function
maps the surface inputs to the material model outputs, and its name carries
the graph's content hash. Bakery compiles it into the project's shader
library: SPIR-V, and Slang to MSL to a metallib. The ADR-077 action cache
keys the result by the generated source. Equal graphs therefore share one
function.

How each backend adds a surface function:

- **Metal (tiled).** The forward fragment inlines the surface function, so
  each Custom graph gets its own fragment functions for the frame variants of
  its alpha bucket. Editor-only inspection variants are left out of packaged
  games.
- **Vulkan (desktop).** The visibility buffer already separates geometry from
  shading. A classification pass counts the pixels of each Custom graph per
  tile. `vk_gbuffer_resolve` skips those pixels. Each Custom graph's resolve
  kernel runs once per frame by an indirect dispatch over its tiles. Standard
  materials keep the single resolve kernel.

| Bucket | Metal pipelines per Custom graph | Vulkan pipelines per Custom graph |
|---|---|---|
| Opaque | 20 in the editor, 16 in a packaged game | 1 compute resolve |
| Alpha-tested | 20 coverage (16 packaged), plus 1 shadow | 1 resolve, plus 1 visibility and 6 shadow (2 cascade, 4 local) |
| Blended | 10 (8 packaged) | 1 forward blend |

**Budget.** A project setting sets the maximum number of Custom graphs. The
starting value is 32, a constant in ADR-096 until project settings hold it.
Phase 4 measured creation on the M1 Pro: up to 4.3 s cold and 0.16 s warm
for each of 7 graphs created in parallel. With 32 opaque Custom graphs, a packaged game
on Metal adds 512 pipeline states to the 50 it has today. The editor warns
when a change would pass the budget. Bakery refuses to package a project
over its budget.

### Why not one shader per material

A material instance is the unit artists make most often, so instances must
cost nothing. A graph is the unit that changes meaning, so a graph may cost a
shader, but only once. No option multiplies across graphs: no static
switches, no per-mesh-type copies and no quality copies. A switch that an
artist needs becomes a parameter, which is a uniform branch, or a second
graph, which counts against the budget.

## Part 4: Shader build and pipeline lifetime

The second rule of Goals and limits holds through these steps.

The path of one edited graph from the editor to the viewport:

```text
artist / agent edits .mtg
        |
        v
editor: document journal group
        |
        v
Bakery daemon (serve)
  lower -> tier
  Custom: codegen, slangc, metal
        |
        v
editor worker: create pipelines
        |
        v
render thread: swap on next frame
  (old version draws until then)
```

- **Edit time.** The editor never compiles on the UI or render thread. The
  viewport keeps the previous version of the graph until the new pipelines
  exist. A Standard-tier edit changes only row data and reaches the next
  frame through `vkr_material_loader_replace_live`.
- **Scene load.** The scene's material list gives its Custom graphs. The
  renderer creates their pipelines on workers before the scene reports
  ready (ADR-096: a background queue, and readiness waits for them).
- **World partition.** A cell's manifest lists its Custom graphs. The
  renderer creates their pipelines before the cell activates
  ([ADR-086](../adr/086-world-partition.md)). Not implemented: a cell's
  Custom materials draw their fallback until ready.
- **Late pipeline.** A draw whose pipeline does not exist yet draws with the
  graph's Standard fallback. It never waits. The fallback is a Standard
  material that Bakery derives from the graph's constant outputs, and the
  artist can replace it. Each late draw increments a frame counter,
  `pipelines_late`. A harness gate requires zero.
- **Packaging.** The project shader library ships in the package. The
  pipeline caches (`MTL4Archive`, `VkPipelineCache`) are device-specific. A
  first run without a cache builds them during scene load, not during play.

## Part 5: The Art workbench

Implemented on 2026-10-09 and recorded in ADR-093: the workbench, the node
canvas, the Material panel with its inspector, the document journal, mesh
material overrides, assignment by palette, operation and drop. Remaining
here: the Lookdev scene, a material picker in Details, and comment frames
on the canvas.

A new built-in workbench, **Art**, sits next to Lighting. It works on the
open level scene, so the viewport is the live preview of every material
edit.

```text
+--------+---------------------+---------+
| Tools  | Scene viewport      | Outline |
|        |                     |         |
|        +---------------------+---------+
|        | Material graph      | Details |
|        | (node canvas)       | (node,  |
|        |                     |  params,|
|        |                     |  cost)  |
+--------+---------------------+---------+
| Content (materials, textures, functions)|
+----------------------------------------+
```

The Tools palette has these groups:

- **SURFACES:** the binding table of Part 6, theme selection, and face paint
  for art-owned overrides.
- **MATERIALS:** new graph, new function, new instance, and assign to
  selection.
- **DRESSING:** decal placement and scatter painting (Part 8).
- **VIEW:** Lit, base colour, roughness, metallic, normals, material cost,
  texel density and greybox.

### Node canvas

The node canvas is a new widget in the UI library, not code inside the
material editor. The animation pose graph and the visual scripting graph of
the [level design toolkit proposal](level-design-toolkit.md) can reuse it.
The widget provides:

- pan and zoom;
- box and Ctrl+click selection;
- typed ports, with wires that refuse a wrong type;
- a search menu to add a node, opened by Space or by dropping a wire on empty
  canvas;
- comment frames;
- per-node error and cost badges;
- copy and paste as `.mtg` JSON text.

### Look development scene

Each workbench keeps its own scene (ADR-089). A project template scene,
`Lookdev`, holds a sphere, a cube, a plane, a 1 m brick wall, the mannequin
and three environment presets. An artist opens it in a custom copy of the
Art workbench to judge a material away from the level. Thumbnails keep using
`vkr_bakery preview material`. This adds no second render path.

### Material assignment

- Details gets a material picker for mesh submeshes and brush faces.
- A material dropped from Content onto an object in the viewport assigns
  it.
- An assignment to a mesh writes an override on the mesh component. The
  mesh asset does not change.

### Document undo

Graph and instance edits go through a document journal. It has the group,
rollback and revert semantics of the scene journal, so agent batches, review
in the Agent changes window and agent-scoped undo work the same on materials
as on scenes (settled 2026-10-09).

## Part 6: The art pass on levels

Implemented on 2026-10-09 and recorded in ADR-094: themes and their
selection by container over the World's, the look order, `world_size` on
brush faces, theme editing in the Material panel and the Art palette,
`surface.*` operations and `art.lint`. Remaining here: the density range as
a project setting, themes reaching bakes from the World, and an `art.lint`
check of the Custom graph budget (Bakery reports it, ADR-096).

### Surface bindings

A **theme** is a project asset (`.surfaces`, JSON) that maps tags to
material instances. A scene selects a theme. A container can select a
different theme, so one district of a level can use worn concrete and
another clean concrete. A tag missing from the theme falls back to the
project's default theme. If the default theme also has no entry, the tag
shows its greybox look.

The binding is by tag, not by face. A designer can therefore move, clip,
extrude or add brushes after the art pass, and new faces take the bound
material at once. Face overrides are for the few surfaces that need a unique
material. An override lives on the face entity and is removed when its face
is removed.

### Brush UVs

A face bound to a material takes its UV scale from the material's
`world_size` unless the face sets its own scale. Bound faces keep Hammer
world-aligned projection ([ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md)).

### Art lint

`art.lint` reports:

- tags with no binding in the scene's theme;
- face overrides that point to missing materials;
- materials whose textures are missing or not cooked;
- texel density outside the project's range on bound faces;
- base colour outside the plausible PBR range (sRGB 30 to 240 for
  non-metals), and metallic values between 0.1 and 0.9;
- Custom graphs over the budget.

## Part 7: Lighting, environment and look

The Lighting workbench gains these tools.

### Environment panel

Implemented in ADR-098, with presets as `.environment` documents.

One panel shows the scene's environment singletons together: atmosphere,
sun and moon, clouds, height fog, volumetric fog, `post_process` and
`time_of_day`. It shows whether each value comes from the scene or the World.
An **environment preset** captures all of them at once. Applying a preset
copies its values into the scene, as component presets do today (open
choice 6).

### Look volumes

Implemented in ADR-097, as below; the viewport does not draw their boxes
yet.

A `look_volume` component overrides part of the look inside a box. The box
comes from the entity transform. Each volume has a priority and a blend
distance. A volume can override these values:

- exposure compensation and the metering range;
- white balance, contrast and saturation;
- bloom intensity;
- height fog colour and density;
- sky-light intensity.

The CPU blends the volumes that contain the camera once per frame. The
result feeds the same globals that `post_process` sets in
[vkr_standard_scene_runtime.c](../../runtime/src/application/vkr_standard_scene_runtime.c).
Look volumes therefore add no GPU pass and no pipeline, and they work on both
pipelines. A scene holds up to 64. `fog_density_box` stays as the volumetric
fog override on the desktop pipeline.

### Lights and bakes

Outlines of point range, spot cones, rectangle lights and volume boxes, the
Lights window, the bake settings form and the Lighting palette's time
scrubber are implemented in ADR-100. Remaining:

- Creation and box handles for reflection probes and diffuse volumes, which
  the editor cannot create today. Probes load from a scene's
  `reflection_probes` array, which the bake also reads, so the bake must
  take probe components first.
- Drag handles on the light outlines.
- A time control on the Scene toolbar.

### View modes for artists

Implemented on the tiled pipeline in ADR-099; the desktop column remains.

| Mode | Tiled (Metal) | Desktop (Vulkan) |
|---|---|---|
| Base colour, roughness, metallic, normals | New, through the inspection variant | The renderer enum has normal and material params; the editor adds them |
| Material cost | New: samples per pixel from the row or graph cost | New |
| Texel density | New | New |
| Exposure false colour | New: scene-linear EV bands after exposure | New |
| Greybox | Part 1 | Part 1 |

## Part 8: Dressing

- **Decal tool.** Implemented in ADR-101: a click places a decal facing
  the surface, and a selected decal draws its box (ADR-100). Decal
  materials as graphs with the `decal` domain remain.
- **Scatter painting.** A brush paints scatter rules onto surfaces with
  density and radius. Splines, spline meshes and `terrain.road` stay in the
  level toolkit, because they define layout. Clutter scatter is art.
- **Vertex paint.** Later work. Layered Standard materials on meshes need
  painted weights. Brush faces use mask, height, slope and world-height
  sources instead.

## Agent operations

Every tool above is an operation in the ADR-084 table with a JSON Schema.
`vkr_mcp` exposes each one as a tool.

| Family | Operations | Writes |
|---|---|---|
| `surface.*` | `list`, `theme.create`, `theme.bind` and `theme.select` are implemented (ADR-094); `define` waits for project tags | Project and scene |
| `brush.*` | `set_surface` (implemented; it sets the surface, the mark or both) | Scene |
| `face.*` | `set_material` (implemented; an empty `material` clears the override) | Scene |
| `material.*` | `list`, `create`, `describe`, `patch`, `set_param`, `assign`, `compile` and `open` are implemented; `preview` remains, and `cost` is part of each lowering answer | Documents and scene |
| `look.*` | `volume` is implemented (ADR-097) | Scene |
| `decal.*` | `place` is implemented (ADR-101) | Scene |
| `env.*` | `describe`, `preset.save` and `preset.apply` are implemented (ADR-098) | Scene and documents |
| `lighting.*` | `list`, `group`, `time` and `bake` with settings are implemented (ADR-100) | Live state and bake jobs |
| Reads | `query.luminance` and `view.capture` with a `mode` (ADR-099), `query.measure`, `view.greybox` and `art.lint` are implemented | — |

The operations that matter most for agents:

- **`material.patch`.** It takes a list of edits: `add_node`, `remove_node`,
  `connect`, `disconnect`, `set` and `expose`. It applies them as one
  document journal group. It returns compile diagnostics keyed by node id,
  the tier with its reason, the cost line and the pipeline change. A
  failed edit rolls back the whole group.
- **`material.describe`.** It returns the graph as compact text, one line
  per node with its connections, so an agent reads a graph without parsing
  the canvas layout.
- **`material.preview`.** It returns PNG images of the material on the
  Lookdev shapes, through the same capture path as `view.capture`.
- **`query.luminance`.** It returns scene-linear luminance and EV at
  labelled marks or over a region. An agent checks "this corridor is 2 EV
  darker than the hall" with numbers, not by reading a picture.
- **`view.capture` modes.** Base colour, roughness, normals, cost, texel
  density, exposure false colour and greybox. These let an agent check a
  material pass in a capture sheet.

A `vkr-art` skill teaches agents the order of work, as `vkr-level-design`
does for levels:

1. Read the theme and `art.lint`.
2. Bind tags before overriding faces.
3. Prefer Standard graphs.
4. Check with luminance queries and capture sheets.

## Phases

Each phase is usable on its own and keeps the rules of Goals and limits.

| Phase | Delivers | Acceptance evidence |
|---|---|---|
| 1. Surface tags and greybox | Implemented (ADR-084) | Recorded in ADR-084 |
| 2. Graph documents and the Art workbench | Implemented (ADR-093); material functions, the Lookdev scene and the Details picker remain | Recorded in ADR-093 |
| 3. Layered Standard and the art pass | Implemented: themes and bindings, `world_size`, face overrides and `art.lint` (ADR-094); layer blends by vertex colour, mask texture, slope or height (ADR-095) | Release Metal timing of Bistro at 2560×1440 on the M1 Pro before and after, with layering compiled in and unused: no regression beyond the run spread. Register counts of the forward variants. A layered test material on Bistro brushes in a capture |
| 4. Custom graphs | Implemented on Metal (ADR-096): code generation, the project library, per-graph variants and buckets, the budget, creation before scene readiness, the fallback and `pipelines.late`. Remaining: Vulkan classification and per-graph resolve, creation at cell load, inspection variants left out of packages | Recorded in ADR-096. Vulkan native checks wait for a Windows host |
| 5. Lighting and look | Look volumes (ADR-097), the environment panel and presets (ADR-098), artist views and `query.luminance` on Metal (ADR-099), bake settings, the time scrubber, outlines and the Lights window (ADR-100) implemented. Remaining: probe and volume creation, outline handles, the Scene toolbar time control, desktop artist views | CPU test of look volume blending at boundaries and priorities. Release Bistro timing unchanged with 8 volumes. Captures of each view mode on Metal |
| 6. Dressing | Decal tool and outline implemented (ADR-100, ADR-101). Remaining: scatter painting | Captures on Bistro; scatter stays within the 4,096-copy bound |

Phase 1 shipped before the other phases and removed the paintable materials
from the level toolkit.

## Risks

- **Pipeline creation time on Metal.** Each Custom graph adds 40 pipeline
  states (ADR-096). On the M1 Pro, creation took up to 4.3 s cold and up to
  0.16 s warm a graph. Other GPUs are unmeasured.
- **Classification cost on Vulkan.** Per-graph resolve adds a
  classification pass and one indirect dispatch per visible Custom graph.
  That cost needs native measurement on a Windows host.
- **Expectations from Unreal Engine.** Vertex offset, custom code and
  per-material shading models are out of scope at first. The editor must
  say why a node is missing, and must not fail without a message.
- **Two names for one surface.** A face has a tag and can have a material.
  The Details panel shows both, with the source of the visible look.
- **Bistro materials.** Phase 2 must not change Bistro's output. The
  migration rewrites the 933 `.mt` files that carry the dead keys, Bistro's
  among them. It must be checked against packed rows, not only against a
  visual check.

## Sources

- [Unreal Engine PSO precaching](https://dev.epicgames.com/documentation/en-us/unreal-engine/pso-precaching-for-unreal-engine)
- [Unreal Engine Substrate materials](https://dev.epicgames.com/documentation/en-us/unreal-engine/substrate-materials-in-unreal-engine)
- [Godot 4.4 ubershaders and pipeline pre-compilation](https://godotengine.org/article/dev-snapshot-godot-4-4-dev-4/)
- [Godot pull request 90400](https://github.com/godotengine/godot/pull/90400)
- [Unity shader variants](https://docs.unity3d.com/Manual/shader-variants.html)
