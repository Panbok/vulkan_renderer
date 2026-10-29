---
status: implemented
updated: 2026-10-29
authority: adr
---

# ADR-137: Baked asset texture sets and model previews

## Status

Accepted. Props made in Blender reach the editor with their own baked
texture sets through the VKR Asset Kit
([tools/blender/vkr_asset_kit](../../tools/blender/vkr_asset_kit/__init__.py)),
and Content shows models as studio thumbnails. `model.list` and the `origin`
of `entity.get`'s mesh tell a tool every placement of a model file.

## Context

The Level Design Test's 66 Blender props drew tiled library materials, one
per slot, with wear and ambient occlusion in vertex colours
([blender-models reference](../../.codex/skills/vkr-art/references/blender-models.md)
before 2026-10-29). Tiled materials repeat at metres; vertex colours carry
wear only as finely as the mesh. Edges stayed hard, chips outlined whole
bevels and nothing showed screws, vents, seams or where hands touch a
prop. The owner asked on 2026-10-28 for AAA-grade props and a toolkit for
artists who make them.

A unique texture set per asset is how shipped games give props that
detail: a high poly baked to a low poly's own UVs, then layered with wear
(Substance Painter's smart materials). VKR already cooks a glTF's images
into host-native textures, streams their mips under a budget (ADR-024),
builds mesh levels of detail on import (ADR-085) and pairs normal and
roughness maps (ADR-012). It discarded authored tangents, which normal
maps baked against MikkTSpace need; the importer now keeps them (ADR-030).

## Decision

1. **Texture sets.** A model's game mesh gets UV map `atlas` (glTF
   `TEXCOORD_0`): Smart UV islands split above the 35 degree smoothing
   angle, so every hard edge is a seam, scaled to one texel density
   (1,024 px/m by default) and packed with a 6-texel margin. A model too
   large for one 2,048 atlas at 640 px/m splits its slots into up to five
   texture sets, largest first into the lightest set it fits. Each set is
   one Principled material with `<set>_albedo.png` (sRGB), `_normal.png`
   (tangent space, glTF +Y), `_orm.png` (occlusion, roughness, metallic,
   also the glTF occlusion texture: VKR reads occlusion from the
   metallic-roughness texture's red channel, and the importer loads the
   shared image once, ADR-012) and `_emissive.png` when it glows. Slots marked Keep (screens whose image each placement chooses)
   keep their own material and UVs.
2. **Bake.** Cycles bakes from the high poly (by default a copy of the game
   mesh) to the atlas: the tangent normal through a Bevel shader that
   rounds every edge, each Principled input routed to emission (albedo,
   roughness, metallic, emission), position, normal, an edge mask (a 6 mm
   bevel against the smooth normal), local cavity (occlusion by the asset
   within 3 cm) and occlusion over a floor plane within 35 cm. First,
   closed parts whose faces point inward (negative signed volume) turn
   outward, since a game mesh draws one side; then faces no ray from the
   outside reaches, and faces looking down from the floor, are deleted, so
   the atlas holds only what shows. Source materials should carry no wear
   of their own: a tiled texture with chips paints them over whole faces,
   where the composer puts wear where it belongs.
3. **Smart materials.** A numpy composer
   ([smart.py](../../tools/blender/vkr_asset_kit/smart.py)) layers each
   slot by its kind (`paint`, `metal`, `plastic`, `laminate`, `wood`,
   `fabric`, `rubber`, `cardboard`, `glass`, `label`, `emissive`,
   `organic`; a
   material's `vkr_smart` property, else its name): grime in cavities and
   near the floor, chips on convex edges (bare metal under paint, with a
   0.35 mm dent in the normal), dust on faces that look up, smudges,
   fingerprints and hairline scratches in the roughness, over Perlin noise
   in object space. An asset's condition scales them (0 new, 1 used, 2
   worn out).
4. **Details.** Screws, rivets, vents, perforations, seams, ribs, touch,
   wear and grime zones and status lights are stamps: a position, a
   surface normal and sizes, written into the texels the position map puts
   on that surface (height into the normal map, colour, roughness, metal,
   emission). They cost no triangles. Scripts give them as an object's
   `vkr_details` JSON; artists add empties parented to the mesh.
5. **Detail maps.** Each texture set takes the ADR-136 detail maps of the
   smart kind that covers most of its area: a tileable normal and grey
   overlay per family (`fine` for paint, plastic and laminate, `metal`,
   `wood`, `fabric`, `rubber`, `cardboard`; none for glass and emissive),
   written once into the project's `assets/textures/vkr_detail/` by
   [detail_maps.py](../../tools/blender/vkr_asset_kit/detail_maps.py). The
   set's glTF material names them in `extras` (`vkr_detail_normal_texture`,
   `vkr_detail_normal_scale`, `vkr_detail_uv_scale`,
   `vkr_detail_albedo_texture`, `vkr_detail_albedo_strength`), which the
   importer writes as the material's detail keys. The UV scale is the
   metres the atlas spans over the family's repeat (4 cm for `fine`), and
   the strengths shrink with the share of the set the kind covers.
6. **Kit.** The add-on's VKR panel bakes and exports
   `<project>/<folder>/<name>/<name>.gltf` with MikkTSpace tangents and no
   vertex colours. Send to Editor connects to the editor's agent socket
   (ADR-084; the default `<temp>/vkr/editor-<uid>.sock`), reimports every
   asset `model.list` reports imported from that content path, or places
   the model at the 3D cursor when none is.
7. **Model previews.** `vkr_bakery preview model --input <mesh.vkb>` reads
   the mesh tool's inspection, which now reports bounds, triangles, range
   count and whether the artifact carries lightmap UVs, scales the model
   to the material studio's unit sphere and renders it from 35 degrees
   front-left and 20 degrees up (recipe `model-studio-v1`). Content shows
   it on Model cards once the model's final textures are encoded.
8. **Agent reads.** `entity.get`'s `mesh` adds the content path the asset
   was imported from (`origin`), its triangles and vertices. `model.list`
   lists the imported models the open scene draws, with asset, origin,
   triangles, submeshes and the entities that draw each, optionally those
   whose origin contains a text.
9. **Details.** Selecting a placed model's root shows its mesh node's Mesh
   section (asset, triangles, bounds) and a Source row: the file it was
   imported from, which shows the asset in Content, and Reimport, which
   imports that file again for every copy. A reimport from Details,
   Content or `model.reimport` without `lightmap_uvs` keeps the lightmap
   UVs the model was cooked with (ADR-076), so the edit-in-Blender loop
   does not leave lit props without them.

## Consequences

- A prop draws one material per texture set instead of one per slot, so
  the Level Design Test's model draws fall from up to eight per placement
  to one or two.
- Texture memory grows per unique model, not per placement: a 2,048 set is
  three 2K maps, about 16 MB cooked with mips before streaming lowers them.
- A reimported model with lightmap UVs keeps its old lightmap until the next
  bake, whose texels no longer match the new UVs, so it draws dark or
  blotched: check looks in the base-colour and normals views and rebake
  after the last reimport.
- The kit bakes the geometry it is given: its texture sets carry detail no
  mesh has, but silhouettes stay the mesh's. The Level Design Test's
  Builder props kept their scripted geometry (bevels of 2 to 8 mm).
- Stamps are projections: a stamp larger than its surface spills onto
  parallel faces within its depth, and texture-space normals from stamp
  heights break where a stamp crosses an island seam.
- Detail maps keep large sets grained at close range, but one family per
  set: a set mixing painted metal with a fabric seat shows the larger
  share's grain on both.

## Alternatives considered

- **Tiled library materials with vertex-colour wear** (the previous
  pipeline). Cheaper in memory, but wear at mesh resolution and no unique
  detail.
- **Substance Painter.** Not scriptable headless here and outside the
  repository; Cycles bakes and the numpy composer run in Blender alone.
- **High-poly geometry for screws and vents.** More triangles per prop for
  detail the normal map carries.
- **An editor-side thumbnail render.** Rejected as a second render path, as
  for material previews; the harness renders both.

## Revisit when

A model needs more than five texture sets, a set needs a detail family per
region (a detail mask from the kinds), or an artist needs UDIMs or
overlapping mirrored UV islands.

## Code evidence

- [bake.py](../../tools/blender/vkr_asset_kit/bake.py) (`bake_asset`,
  `plan_groups`, `unwrap_atlas`, `Routing`),
  [smart.py](../../tools/blender/vkr_asset_kit/smart.py) (`compose`,
  `STAMPS`), [detail_maps.py](../../tools/blender/vkr_asset_kit/detail_maps.py),
  [link.py](../../tools/blender/vkr_asset_kit/link.py),
  [check_asset_kit.py](../../tools/blender/vkr_asset_kit/check_asset_kit.py)
- [vkr_mesh_cooker.c](../../tools/vkr_mesh_cooker.c) (inspection bounds),
  [editor_scene_panels.c](../../editor/src/editor_scene_panels.c)
  (`inspector_mesh_source`),
  [vkr_bakery_bake.c](../../tools/bakery/vkr_bakery_bake.c)
  (`vkr_preview_model_framing`),
  [editor_content.c](../../editor/src/editor_content.c) (`content_preview`),
  [editor_ops.c](../../editor/src/editor_ops.c) (`ops_run_model_list`)

## Evidence

On the M1 Pro (Metal, Release), 2026-10-28 to 2026-10-29:

- `Blender -b --factory-startup --python
  tools/blender/vkr_asset_kit/check_asset_kit.py` (Blender 5.2.2) passed:
  an inside-out part turns outward and an open card keeps its winding;
  hidden faces go; MikkTSpace tangents and no `TEXCOORD_1` or `COLOR_0`;
  the screen keeps its slot; the screw stamp lands as metal; the status
  light writes an emissive texture; a large model splits into sets of
  their own density; and the add-on's Bake & Export, registered as the
  panel runs it, bakes a box with an Add Detail screw into the panel's
  project, names that project's detail maps and leaves the scene's object
  unchanged.
- `--suite gltf` of the Release tester passed, with the authored-tangent,
  detail-extras and shared-ORM tests.
- The Level Design Test's 66 Builder props (977 placements) were baked
  through the kit and reimported. Contact sheets of the kit's previews
  found two input defects the kit now reports or fixes: tiled worn paint
  put chips in the middle of faces (the Builder driver bakes paint flat),
  and 26 inside-out leaf blades and 4 server rack parts vanished (now
  turned outward). The coplanar check found the first-aid cross and the
  keyboard's space bar, fixed in the Builder. After a full bake (lightmap
  samples 64, the level's bounded diffuse volume) the office, locker room,
  server room, warehouse racks and crate store drew lit with their sets.
- The Content tile of `server_rack` showed its `model-studio-v1`
  thumbnail; Details showed a placed drum's Mesh section and Source row,
  whose click opened its Content folder with the asset selected.
- A reimport with the Bakery panel's lightmap UVs off kept the pallet
  rack's lightmap UVs when `lightmap_uvs` was omitted; the keyboard
  reimported with `false` had none and kept none.
- Not measured: an authoritative timing pair. In the editor viewport
  (about 1,080 by 720 rendered pixels), five still views drew
  `gpu.submission` medians of 4.1 to 5.1 ms with the baked props.

Not run: Vulkan. Vulkan imports the same glTF and keys; no desktop capture
was taken.
