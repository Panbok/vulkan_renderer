---
name: vkr-art
description: Author VKR materials and do the art pass through the editor's agent channel - material graphs (.mtg), instances and plain .mt files, surface themes that bind tags to materials, assigning materials to brush faces and meshes, modelling props in Blender and placing, reimporting and dressing them as models, decals and the environment detail pass (signs, markings, wear, wall and ceiling fixtures), and checking the result with art.lint, lowering reports and captures. Use for material and look work in a level; use vkr-level-design for layout, surfaces and marks, and vkr-editor-cmd to start or drive the editor.
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
4. Read the objects' design tags (`tag.vocabulary` for their meaning,
   `tag.list` with a `region`, `scene.describe` `tags`): material, finish,
   color and condition tags state the look the final asset needs, as
   `#painted_metal #blue #clearcoat #worn`, and the space tag its palette.
   Make the material each combination asks for (a clearcoat layer for
   `#clearcoat`, roughness from the finish, wear from the condition), and
   report objects whose tags contradict their surface instead of guessing.

## Documents

- A graph (`.mtg`) holds the nodes; an instance (`.mt` with `graph=` and
  `param.<name>=` lines) overrides the graph's exposed parameters; a plain
  `.mt` is a definition. Prefer one graph per family of looks (brick, painted
  metal) and an instance per variant; never copy a graph to change a colour.
- Create with `material.create`: `kind` graph (a template, or `from` a PBR
  `.mt`, whose texture paths become content-root relative), instance
  (`graph`, `params`, `name`) or definition (`from` another, or `name` and
  `params` naming definition keys: `base_color` [r, g, b, a],
  `emissive_factor`, `roughness`, `surface`, texture paths; a definition
  that does not read is refused). Paths are content-root relative
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
  reloads it; the material shows its fallback meanwhile. On a Mac without
  the Metal Toolchain the renderer compiles the library's source itself,
  which takes about 5 s more; the log reads "compiling project materials".
- A normal map may read math over normal textures, such as the mean of two
  ripple layers scrolled against each other over `time`, which hides the
  repeat of one texture on large water. Wait with
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
- Most levels need more looks than the 13 surface tags: a lab wall and a
  station wall are both `plaster` or `none`. Bind those by the objects'
  design tags with rules: `rules` (replace all), `rules_add` or
  `rules_remove`, each `{"tags": ["#lab", "#wall"], "material": ...}` with
  optional `surface`, `mark` and `faces` (`floor`, `wall`, `ceiling`). A
  brush without tags takes its nearest tagged ancestor's. The rule with the
  most conditions wins, the earlier on a tie, and every rule beats a plain
  binding. Write rules as data in a script: one per material tag, then
  colour, condition and object pairs, then space triples; `faces` splits a
  blockout room's walls from its floor and ceiling. Pair an orientation
  only with an object tag (`#wall`, `#stairs`): `#lab` with `floor` would
  tile every desk top.
- `surface.list` answers `greybox_faces` per tag and in total, and each
  rule's `dressed` face count: drive `greybox_faces` to 0 and read rules
  with 0 as shadowed by an earlier rule of the same score.
- Bindings, rules and `material.assign` refuse a material that does not
  open: create materials first.
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

## Decals

- `decal.place` puts a decal facing a surface: `position` and `normal`, or
  `origin` and `direction` to place it where that ray meets collision
  (imported meshes such as Bistro's have none; brushes and terrain do).
  `size` is the width and height across the surface, `depth` its reach
  along the normal, `angle` a turn in degrees, and `material` a `.mt` file.
- The image's top points up a wall. On a floor it points along `facing`,
  which defaults to the ray: an arrow decal points where `facing` points.
  A long floor line is a thin decal (`size` [length, 0.14]) turned with
  `angle` 90 across `facing`. Capture lettered decals to check them.
- A decal lays only base colour, weighted by its texture's alpha, over every
  opaque surface inside its box (ADR-092): no normal, roughness or glow. Draw
  each image as an RGBA PNG with a transparent background (signs, stencils,
  floor paint, grime), wear paint by multiplying alpha with noise, cook it
  with `vkr_bakery cook`, and give it one plain definition through
  `material.create` (`base_color_texture`, `base_color_colorspace` srgb,
  white `base_color`). A hand-written `.mt` that does not parse draws nothing.
  Keep the decal's `size` at the image's aspect.
- Keep boxes thin (`depth` 0.15 to 0.2 on walls, 0.3 on floors) and off
  props: the box paints desks, rails and monitors inside it too.
- The frame draws the 64 decals nearest the camera; keep a space under about
  40. On the tiled pipeline any decal in view costs about 0.5 ms of
  `Tiled.Opaque` however small.

## Environment detail

A finished room reads as used: markings, signs and wear on its surfaces and
fixtures on its walls and ceiling. Dress each space by script, after the
models:

1. Survey the spaces: from each lamp, `query.raycast` down to the floor, up
   to the ceiling and out to the four walls. Rays stop at props' clip boxes
   and door leaves; step past hits whose names are not structure.
2. Read what is already there: `text` entities and sign brushes name rooms
   and directions. Do not repeat or contradict them; wayfinding follows the
   level's real routes and sits above wainscot and handrails.
3. Place by ray, then check clearance. Placed models have no collision, so
   test each wall decal's box against the bounds of models, fixtures and
   text before placing it, and slide it along the wall or skip it. Check
   that the wall stands behind both ends (no doorway) at the decal's height.
4. Per space: wear along each wall's base, a few floor scuffs or stains,
   streaks and damp in service areas; posters, noticeboards and whiteboards
   where people work; hazard, lane and keep-clear markings where vehicles
   run; exit signs over egress doors. Then a functional layer keyed to the
   fixtures' poses: keep-clear zones in front of electrical panels, red
   zones under extinguishers, dock boxes in front of chargers, flow labels
   on each pipe's front (its offset plus radius from the wall), hand marks
   beside door handles and drains in wet rooms (never on carpet).
5. Wall fixtures (alarms, first aid, cameras, panels), ceiling services
   (detectors, sprinklers, speakers, one model per 3 m strip, offset from
   the light panels) and runs (cable trays, pipes) are Blender models ([references/blender-models.md](references/blender-models.md)).
   Place small fixtures without `lightmap_uvs`, so no rebake is needed.
   Hide blockout shapes the same way: a louvred troffer just under each
   flat emissive light panel (the panel keeps glowing and stays in the
   bake), a round casing over an octagonal or square column, and cove
   skirting runs along corridor walls (2 m segments, skipped where a
   doorway opens).
   Validate each run segment: the wall behind its ends and middle, and the
   room tall enough for it.
6. Names repeat across runs and `entity.delete` by a repeated name fails:
   delete by id, and keep each object under a prefix (`env/<space>/...`) so
   a pass can be found and replaced.
7. Capture every space after the pass, then save and reopen to check that
   the pass persisted.

## Scatter

- Dress a street with clutter by painting a scatter, not by placing copies
  one by one. Create the rule with `scatter.create` (mesh, scale range,
  `align_to_surface`, `random_yaw`), then `scatter.paint` with `scatter`,
  world `points` along the stroke, `radius`, `density` (copies per square
  metre) and `spacing`. Once a scatter has painted areas it places copies
  only in them.
- `erase` with points deletes the areas under them. Each call is one undo
  step.
- Copies need collision or terrain below. `entity.get` on the scatter
  answers `copies` and a `status` that says how many found no ground or no
  room. A scene holds at most 4,096 copies, and a scatter 2,048.

## Models

Replace a level's brush prototypes with modelled props in this order; the
Blender side is in [references/blender-models.md](references/blender-models.md).

1. Measure each prototype from `scene.describe` (parts, bounds, tags): its
   footprint, height and front (the side its user stands at: a desk's edge
   band, a locker's door, a chair's seat away from its back). Group parts by
   name prefix; write the frames to a file a script reads.
2. Model each family once at its typical size, front at Blender -Y, origin
   on the floor at the footprint centre, and export glTF.
3. `model.place` every copy in one call (at most 512): `rotation` [0, yaw,
   0] turns the model's +Z to the prototype's front, `scale` fits its box
   (keep chairs, monitors and small items uniform), `position` puts the
   model's bounds centre on the footprint centre, `tags` the prototype's.
   Or give a model `fit` {`min`, `max`, `mode`}: the editor measures the
   glTF and puts it into that world box (`stretch` per axis at yaws that
   are multiples of 90, `uniform` keeps proportions), standing on its
   floor. Pass `lightmap_uvs` true for large props when the level is
   baked; small items draw well with the diffuse volume. Save first; the
   job reloads the scene, so wait (`wait.scene`) and read entities again:
   ids change on every reload. The placement is one undo step.
4. Delete the prototype brushes and add one `clip` `brush.box` per prop over
   its footprint, so the player still collides. Send at most about 100
   deletes per batch: a larger one runs out of undo memory and rolls back,
   and a rolled-back batch gives the restored brushes new ids.
5. Give each slot a library material (below), then rebake.

- The mesh hangs on a child node of the placed root: `material.assign`
  that child with `slot` to give one surface a library material. Slots
  follow the glTF's primitive order, and only slots 0 to 7 take an
  override: give a model at most eight. Assign every slot of a mesh in one
  batch: the edits merge into one override. Pass `select` false on batches
  that should leave the designer's selection alone.
- In the editor, the Details panel's Materials section lists the slots of
  the selected mesh (or of a placed model's mesh node): drag a material
  from Content onto a slot, click it to open the material, clear it to
  return to the model's own.
- `entity.get` on that child answers `mesh` with its `asset` reference.
  After editing the source, `model.reimport` with the `asset` or a placed
  `entity` rebuilds the asset for every copy without a reload, from the
  file it was imported from (or a new `source`); wait for the job before
  the next one, and pass `lightmap_uvs` again or the rebuilt mesh loses
  them. The saved edits on its nodes follow the new mesh, and the scene
  shows unsaved: save before a bake. A mesh with lightmap UVs keeps its
  old lightmap until the next bake, which draws a changed model dark or
  blotched: rebake after reimporting lit props. Reimporting from a
  different model keeps the slots by primitive order, so assign them
  again from the new file's slot names.
- After any reload, check `editor.status` `scene`: `kept_edits` counts
  saved edits that bind to nothing (kept and written back unchanged), and
  `save_blocked` with `overlay_problem` means the edit file did not load;
  the editor then refuses to save, so fix the file before editing more.
- A glTF `COLOR_0` multiplies a plain material's base colour, so bake
  ambient occlusion into it there (white where nothing should darken); on
  surfaces that take a layered material with the
  `vertex_color` mask, RGBA weigh its layers 0 to 3. A painted-metal graph
  whose layer 1 is chipped steel and layer 2 grime, with the weights baked
  in Blender (edges and floor height), wears every prop without textures
  of its own.
- A screen or sign that shows an image: give its face 0..1 UVs in Blender
  and a Standard graph whose emissive is a glow colour times an exposed
  `screen` texture (no pipeline), one instance per image, chosen per
  object by room. Only animation (scanlines over `world_position`, a crawl
  over `time`) needs a Custom graph; one with `tile_offset` (scale 1/size,
  offset -min/size) can fit a face's metric UVs instead.
- `entity.delete` on a placed model's root deletes it with its nodes, and
  undo restores it; `undo` after `model.place` removes the whole placement.
- To replace a family that is already modelled (a refit), re-export to the
  same file and `model.reimport`; to replace brush props, script the
  frames as above with raycasts (`query.raycast` at wall height, rays
  above table tops) to put wall items such as clocks, displays and badge
  readers on the wall face, back to the wall.
- Delete a brush prop by its group root's id: its child brushes go with it.
  Then `scene.describe` the prop's region again; pieces that were never
  parented (a rack's light strip) stay behind. Removed brushes leave
  their baked shadows on walls and floors until the next bake.
- One model can take a colour per placement: assign a different library
  instance to its colour slot on each copy (drums in worn blue, grey and
  beige). A model with more than eight materials keeps its own glTF
  materials past slot 7: build library surfaces first and leave the extra
  slots to colours no library material needs (food in a servery's pans).
- Mount wall screens 1 to 2 cm proud of the wall. Three 1.9 m menu screens
  placed flush on the cafeteria wall did not draw, and 1 cm off they did
  (the cause is not yet found).
- If a placement job fails, the Scene shows "Preparation failed" and
  `editor.status` answers `scene.loaded` false: do not save. Reopen the
  scene, check the entity count and the edits that ran before the job, and
  place again; one such failure (a validation child exiting 127) passed on
  the retry.

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
  default. Lightmap bakes are incremental: they reuse the last set when
  nothing changed and relight only what edits reach, and an unchanged
  diffuse volume is kept. `full: true` rebakes both; use it when an
  incremental result looks wrong and before shipping.
- In a project scene, `probe.create` (`center`, half `extents`) adds a
  reflection probe for each room or street whose reflections should
  differ. Save the scene, then `lighting.bake` captures it; move or resize
  it later with `component.set` on `reflection_probe`. A scene holds 16.
- `diffuse_settings` `bounds_min` and `bounds_max` limit the diffuse volume
  to a box around the interior; both zero bake the whole scene.
- A lightmap bake of a large level (the 50,000-object Level Design Test:
  28 minutes, 4 GB) can be killed for memory beside a running editor (the
  job reports "Bake cancelled"). Save, quit the editor, and run the job's
  request alone: copy `.vkreditor/jobs/<id>/request.json` to a new folder,
  set its `runtime_directory` to that folder, and run `vkr_bakery project
  --request <it> --result <folder>/result.json`. The scene loads the new
  lightmaps when it next opens.
- Models placed without lightmap UVs draw with the diffuse volume and
  reflection probes instead of a lightmap. Rebake after replacing or
  deleting props: the old lightmaps keep the shadows of what was there.
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
3. Capture the assigned surfaces (`view.capture` with `eye` and `target`)
   after Cmd `grid off` and `view.tool_brushes off`, which hide the grid
   and the clip and trigger brushes the game never draws;
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
