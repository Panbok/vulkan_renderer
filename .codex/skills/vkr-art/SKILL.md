---
name: vkr-art
description: Author VKR materials and do the art pass through the editor's agent channel - material graphs (.mtg), instances and plain .mt files, surface themes that bind tags to materials, assigning materials to brush faces and meshes, and checking the result with art.lint, lowering reports and captures. Use for material and look work in a level; use vkr-level-design for layout, surfaces and marks, and vkr-editor-cmd to start or drive the editor.
---

# VKR art pass through the agent channel

[docs/proposals/artist-toolkit.md](../../../docs/proposals/artist-toolkit.md),
[ADR-093](../../../docs/adr/093-material-graphs-and-art-workbench.md),
[ADR-094](../../../docs/adr/094-surface-themes-and-art-pass.md) and
[ADR-095](../../../docs/adr/095-layered-standard-materials.md) define the
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

1. `material.compile` each graph and instance you changed: `tier` standard,
   `pipelines` 0, and `samples` within your budget.
2. `art.lint` the container: no `unbound_tag` the level shows, no
   `missing_material`, `missing_texture` or `uncooked_texture`; fix
   `texel_density` (default 128 to 2048 px/m; pass the project's range) by
   `world_size`, and `base_color` or `metallic` values outside plausible
   PBR ranges.
3. Capture the assigned surfaces (`view.capture` with `eye` and `target`);
   compare `view.greybox` on and off to confirm which faces carry art.
4. Changes you make for review (`review` default true) appear in
   `changes.list` with their `document`; the designer accepts or rejects
   them. `undo` takes only your own steps, documents included; name yourself
   in the request or it refuses.
5. Report the documents you created or changed, their tiers, the themes
   each container takes, what `art.lint` still reports and the change ids.
