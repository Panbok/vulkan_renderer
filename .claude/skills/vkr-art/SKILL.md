---
name: vkr-art
description: Author VKR materials and do the art pass through the editor's agent channel - material graphs (.mtg), instances and plain .mt files, surface themes that bind tags to materials, assigning materials to brush faces and meshes, and checking the result with art.lint, lowering reports and captures. Use for material and look work in a level; use vkr-level-design for layout, surfaces and marks, and vkr-editor-cmd to start or drive the editor.
---

# VKR art pass through the agent channel

[docs/proposals/artist-toolkit.md](../../../docs/proposals/artist-toolkit.md),
[ADR-093](../../../docs/adr/093-material-graphs-and-art-workbench.md),
[ADR-094](../../../docs/adr/094-surface-themes-and-art-pass.md),
[ADR-095](../../../docs/adr/095-layered-standard-materials.md) and
[ADR-096](../../../docs/adr/096-custom-material-graphs.md) define the
model; `ops.list` returns every
operation's schema. This skill sets the order of work and the checks.

## Start

1. Connect and name yourself as `vkr-level-design` describes (socket,
   `agent` in every request, read `editor.status` and `changes.feed`).
2. Read before writing: `surface.list` (the tags the level uses, with face
   counts and current bindings), `art.lint`, `material.list` (`contains`
   filters by path) and `material.describe` of each document you will
   touch.
3. Leave level layout alone: brushes keep their `surface` and `mark`. The art
   pass sets themes, art-owned materials and material documents.

## Documents

- A graph (`.mtg`) holds the nodes; an instance (`.mt` with `graph=` and
  `param.<name>=` lines) overrides the graph's exposed parameters; a plain
  `.mt` is a definition. Prefer one graph per family of looks (brick, painted
  metal) and an instance per variant; never copy a graph to change a colour.
- Create with `material.create`: `kind` graph (a template, or `from` a PBR
  `.mt`, whose texture paths become content-root relative), instance
  (`graph`, `params`, `name`) or definition. Paths are content-root relative
  (`assets/materials/...`); an existing file needs `"overwrite": true`.
- Texture paths inside a graph resolve against the graph with `./` or `../`,
  else against the content root. A texture path given as an instance
  parameter resolves against the instance.
- Edit graphs only with `material.patch` (`add`, `remove`, `connect`,
  `disconnect`, `set`, `settings`). Give node ids yourself, so later edits
  name them. A failing edit names `edits[i]` and changes nothing.
- Edit instances with `material.set_param`; `null` removes an override.

## Stay in the Standard tier

A Standard graph lowers to factors, textures and state, so it costs no
shader and no pipeline. `material.patch`, `material.describe` and
`material.compile` answer `lowering` with `tier`, `reason`, `node`,
`samples` and `pipelines`. Keep `tier` at `standard`:

- A surface input takes a constant, a texture output, or a `multiply` of one
  constant and one texture output; a normal input takes a `normal_map`.
- Scalars read fixed texture channels: metallic `b`, roughness `g`,
  occlusion `r` (one ORM texture feeds all three), clearcoat `r`, clearcoat
  roughness `g`, sheen roughness `a`, anisotropy `b`, transmission `r`,
  thickness `g`. Metallic and roughness must read the same file.
- Two textures multiplied, other channels or other node chains are not
  Standard; fix the graph by the `reason` and `node` the answer names.
- Expose a value as a `parameter` only when instances vary it.

## Layers

Weathering (moss on tops, grime low on walls, wear patterns) is a layered
material, not a second texture multiplied in:

- Make each layer an ordinary `.mt` (an instance is fine) with its base
  colour, normal and ORM. Add a `layer` node (`path` to it) per layer, up to
  three, into a `layer_blend` node, and connect `blend.layers` to
  `output.layers`.
- Pick the blend's `mask`: `slope` with `value` [from, to] in world normal Y
  (0.6 to 0.8 puts layer 1 on what faces up), `height` with [from, to] in
  world meters, `texture` with `path` to a mask whose RGBA weigh layers 0 to
  3, or `vertex_color` for painted meshes.
- Layered materials are opaque. Layers share the base's UVs and its
  `world_size`. Bakes see layer 0 only.
- `lowering.layers` counts them; each layer adds three samples.

## Custom graphs

Make a Custom graph only for what the Standard tier cannot express:
animation over `time`, `world_position` or `world_normal` effects, tiling
math on `uv`, `world_planar` projection or arithmetic between textures.
Each Custom graph costs 40 pipelines (`lowering.pipelines`), counts against
the project budget of 32 and takes seconds to compile, so:

- Vary instances, not graphs. Expose a constant as a `parameter` (up to 8)
  and texture paths per instance; a changed literal is a new graph.
- Wire the constants an artist should see before the pipelines exist
  straight to the surface output: they form the fallback look.
- Outputs are base colour, opacity, metallic, roughness, occlusion, normal
  and emissive. Clearcoat, sheen, anisotropy and transmission stay
  Standard. There is no vertex offset and no code node.
- After a write, Bakery recompiles the project library and the renderer
  reloads it; the material shows its fallback meanwhile. Wait with
  `wait.scene` (it waits while `stats.pending_pipelines` is above zero)
  before captures. `stats.pipelines_late` counts camera draws that used a
  fallback.
- Custom graphs draw only on Metal: Vulkan shows the fallback, and so do
  blended Custom materials.

## Bind tags first

A theme (`.surfaces`) binds each surface tag to a material for every face
of the tag that has no material of its own, including faces designers add
later. Bind by tag; assign per face only for the few surfaces that need a
unique look.

- `surface.theme.create` (`path`, `materials`: tag to `.mt`), then
  `surface.theme.select` (`theme`, `container`) makes a container take it.
  The World's theme (`container` "world") applies under every scene's: put
  the project's defaults there and a district's differences in its own.
- `surface.theme.bind` changes bindings: `tag` and `material`, or
  `materials`; an empty material unbinds. Without `path` it edits the
  container's own theme.
- Size textures in meters, not per face: give each material a `world_size`
  (meters one texture repeat covers) with `material.set_param` on an
  instance or `material.patch` settings on a graph. A face's `uv_scale` then
  counts repeats of it, so leave faces at 1 unless one needs a different
  scale. Set `surface` on materials meshes use, so they share the tags.

## Environment

- `env.describe` lists each part of a container's environment (sky light,
  atmosphere, clouds, fog, volumetric_fog, post_process, time_of_day, sun,
  moon) with where it comes from: the `container`, the `world` or `unset`.
  Read it before changing the look; World parts are shared by every level.
- `env.preset.apply` (`path` to a `.environment`) copies a preset's values
  into the container as one undoable batch; `env.preset.save` saves the
  current one. Presets are copies: put values every level shares in the
  World instead.

## Look volumes

Give a room, tunnel or district its own exposure and grading with a look
volume, not by editing the scene's `post_process` (which changes the whole
level):

- `look.volume` takes world `min` and `max` corners, `blend_distance` in
  metres (fade outside the box, default 1), `priority` and a `look` object.
  Name only the values to change: `exposure_compensation_ev`, `metering`
  [min, max] EV, `white_balance` [temperature, tint], `contrast`,
  `saturation`, `bloom_intensity`, `fog_color`, `fog_density`,
  `sky_light_intensity`.
- Nest volumes by priority: the room's above the district's.
- Volumes apply at the camera, so check each with a capture from inside it
  and one past its blend distance.

## Lights and bakes

- `lighting.list` returns the scene's and the World's lights (`kind`,
  `from`, component values with `mobility`, `light_group` and intensity)
  and the scene's light groups. Read it before relighting. Edit a light
  with `component.set` on its `point_light`, `rectangle_light` or
  `directional_light`.
- Static lights bake into lightmaps, and the tiled pipeline adds no
  runtime light for them. After changing static lights, run
  `lighting.bake` on a project scene. Its `lightmap_settings` and
  `diffuse_settings` hold only the values to change; 0 keeps Bakery's
  default.
- `lighting.time` (`hour`) and `lighting.group` (`group`, `intensity`)
  change the live clock and a group's level until the simulation resets.
  Use them to preview and capture a mood. To keep an hour, set the World's
  `time_of_day` `hour`.

## Assign

- `material.assign` gives a `face`, a `brush` (all faces, or those `faces`
  selects: `top`, `sides`, `+x`...) or an `entity` a material: a brush's
  faces, else a mesh's submesh `slot` (0 to 7) or every submesh, through the
  entity's `material_override` (the shared mesh asset keeps its own). An
  empty `material` clears. Imported scenes put meshes on child nodes, not
  on the root: `dry_run` finds which nodes take a material.
- Brush and blockout operations reject `material`: layout keeps surfaces.
- Show your work to the designer with `material.open` (`workbench` true
  switches to the Art workbench).

## Check, then report

1. `material.compile` each graph and instance you changed: `tier` standard
   with `pipelines` 0, or `custom` only where the section above allows it,
   and `samples` within your budget.
2. `art.lint` the container: no `unbound_tag` the level shows, no
   `missing_material`, `missing_texture` or `uncooked_texture`; fix
   `texel_density` (default 128 to 2048 px/m; pass the project's range) by
   `world_size`, and `base_color` or `metallic` values outside plausible
   PBR ranges.
3. Capture the assigned surfaces (`view.capture` with `eye` and `target`);
   compare `view.greybox` on and off to confirm which faces carry art. A
   sheet of `views` with `mode` set to `base-color`, `roughness`,
   `metallic`, `normals`, `material-cost` or `texel-density` shows the
   material data without lighting (Metal; Vulkan draws them lit). Texel
   density is blue below 128 px/m, green inside, red above 2048.
4. Check light with numbers: `query.luminance` with labelled `marks` or a
   `region` answers `luminance` (compare places with it; exposure does not
   change it) and `stops` from middle grey after exposure (judge the frame
   with it; the `exposure` mode shows the same bands as colour).
5. Changes you make for review (`review` default true) appear in
   `changes.list` with their `document`; the designer accepts or rejects
   them. `undo` takes only your own steps, documents included; name yourself
   in the request or it refuses.
6. Report the documents you created or changed, their tiers (with each
   Custom graph's reason), the themes
   each container takes, what `art.lint` still reports and the change ids.
