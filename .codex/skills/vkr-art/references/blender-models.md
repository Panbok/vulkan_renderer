# Modelling props in Blender for VKR

Load this when you build or change a model for a VKR level. The parent skill
owns placement, materials and bakes.

## Run Blender by script

- Run Blender headless, one process at a time:
  `/Applications/Blender.app/Contents/MacOS/Blender -b --factory-startup -P
  <script.py> -- <asset names> [--preview]`. Each asset is a Python function
  that builds one object; the script exports the ones named.
- Build from data: take the prototype's measured size as the function's
  default arguments, so a family rescales without remodelling.
- Keep scripts and previews in `.scratch/`; export models to a gitignored
  content folder such as `assets/models/<level>_props/<asset>/<asset>.gltf`.
  Content paths, not absolute ones, go to `model.place`.

## Geometry

- Model in metres with Z up. The user stands at -Y: a desk's edge band, a
  locker's door, a monitor's screen, a chair's seat front. glTF turns -Y
  into the engine's +Z, which `model.place`'s yaw then points.
- Put the origin on the floor at the footprint centre. Put a monitor's or a
  lamp's origin under its base, so a placement on a desk top needs no
  offset. Put a wall fixture's origin on the wall at floor height, its back
  on the wall and its front toward -Y, with the fixture at its usual height
  (a call point at 1.3 m): one ray to the wall and one to the floor place
  it. A ceiling run (cable tray) hangs from an origin on the ceiling; a
  wall run (pipes) runs along X so segments line up end to end.
- Build from bevelled boxes, frustums and cylinders through `bmesh`; merge
  doubles, smooth by angle (about 35 degrees) and apply a Weighted Normal
  modifier so bevelled edges catch light without a normal map.
- Avoid the boxy, low-poly look: no hard box edge on any part thicker than
  about 12 mm (a 3 to 4 mm bevel at least), cylinders at about 1.5 cm per
  side, and real curves where furniture has them. In `bm_blender.Builder`:
  `soft_box` (support loops plus one subdivision level: cushions, casings,
  rounded tops), `shell` (a bent, bulged and subdivided panel: chair seats
  and backs, moulded shells), `limb` (tapered capsules: legs, tube frames,
  arms), `blade` (pointed, curving leaves) and `sphere`. Square legs and
  flat slab seats read as prototypes; round tubes and curved shells read
  as products.
- Keep triangles where they show: subdivide only parts over about 8 cm
  (smaller ones take a plain bevel), and profile a model's triangles per
  call before export. A task chair of curved shells costs about 19,000
  triangles and a full HEV suit about 23,000; most furniture stays under
  10,000.
- Light-grey plastics read near white under bright overhead light: give
  chair frames and seat pans a black or dark material.
- Budget: a few hundred to 4,000 triangles for furniture; a keyboard's
  keycaps reach 9,000. Several hundred copies of 2,000 to 6,000 triangles
  cost the Level Design Test nothing measurable.
- Overlap parts by at least a few millimetres or keep them 1 cm apart:
  coplanar faces z-fight in the engine.

## Materials and texture sets

Bake every prop's own texture set with the VKR Asset Kit
(`tools/blender/vkr_asset_kit`, [ADR-137](../../../../docs/adr/137-baked-asset-texture-sets-and-model-previews.md));
library materials on slots and vertex-colour wear are the old pipeline.

- Give each surface its own Principled material: base colour (a texture
  times a tint is fine), roughness, metallic, a normal map, emission for
  lights. Name it after what it is (`paint_blue`, `aluminium`, `oak`,
  `rubber_black`, `glass_black`, `labels`): the name picks the smart kind
  (paint, metal, plastic, laminate, wood, fabric, rubber, glass, label,
  emissive, cardboard, organic) unless the material's `vkr_smart` property
  names one. Painted metal chips to bare steel; plastic and laminate only
  lighten at the edges, so name moulded plastic accordingly. Use clean
  base textures: a tiled worn-paint texture puts chips in the middle of
  every face, where the kit puts wear on edges and where hands reach.
- Keep every closed part's faces pointing out. The kit turns inside-out
  closed parts outward and reports `flipped_parts`; open cards keep their
  winding and draw one side.
- UVs on the high poly can be anything the materials need (metric box
  projection for tiling textures). The kit gives the game mesh its own
  atlas (UV0) at about 1,024 px/m, every hard edge a seam, and splits a
  large model into up to five texture sets so none drops under 640 px/m.
- Screens (`display`, `display_b`) and see-through glass (`glass_tint`, or
  any material with alpha below 1 or transmission) are kept: they export
  with their own material and UVs. Give screen faces 0..1 UVs (U along the
  width as seen from the front, V up) and assign the image per placement;
  assign the library's blended glass (`assets/materials/bm/glass.mt`) to
  kept glass so shelves behind it show. Kept slots do not hide faces behind
  them from the hidden-face pass.
- Details are stamps, not geometry: give a scripted asset's object a
  `vkr_details` JSON list, or parent empties with a `vkr_detail` kind
  (Add Detail in the kit's panel). Kinds: `screw` (radius, drive
  phillips|slot|hex, finish steel|black|zinc|brass|chrome), `rivet`, `vent`
  (width, height, slots, slot_width), `perforation` (pitch, hole_radius),
  `seam` (a cutting plane `point`/`cut` inside a `min`..`max` box), `ribs`,
  `touch` (hands: smudged paint, polished metal), `wear` (scuffs), `grime`,
  `glow` (status lights) and `condition` (0 new, 1 used, 2 worn out). Put
  touch zones where hands go (handles, keyboards, armrests, buttons), wear
  where feet and forklifts hit, grime where dirt gathers.
- Run it headless through `vkr_asset_kit.bake.bake_asset(obj, name,
  out_dir, Settings(project_root=...), details=...)`, as
  `check_asset_kit.py` does, or from the 3D Viewport's VKR panel. It
  writes `<name>.gltf` with MikkTSpace tangents (VKR keeps them, ADR-030),
  `<set>_albedo/_normal/_orm/_emissive.png`, and detail map extras (fine,
  metal, wood, fabric, rubber, cardboard maps in
  `assets/textures/vkr_detail`) that the importer turns into the
  material's `detail_*` keys (ADR-136).
- One Blender process at a time; each bake takes 30 s to 3 min and up to
  3 GB on the GPU. The first Cycles run compiles kernels for about 2 min.

## Labels and soft parts

- Printed stickers and plates come from one atlas texture of square cells
  (logo, hazard signs, barcodes, rack and room plates, serial plates): a
  thin box of a `labels` material per sticker, a face integer attribute
  naming its cell, and UVs normalized over each face into that cell with
  a 2% inset. Make the sticker's aspect match the cell's and stand it 1 to
  2 mm proud of the surface; the bake prints it into the texture set.
- Look up `bmesh` custom layers by name after every merge
  (`bm.faces.layers.int["cell"]`): `from_mesh` can add layers and a handle
  kept from before then reads another layer, so labels added before a
  cable showed the wrong cell.
- Figures and displays inside enclosures: leave the front open or keep
  glass to a thin band, because tinted glass hides what the enclosure is
  for (an HEV suit in its pod). `Builder.sphere` makes helmets and domes.
- Open containers (bins, cups) need open walls: a capped cylinder closes
  them. Build a ring of outer and inner walls with lips.
- Soft furnishings are boxes bevelled by 40 to 50% of their smallest size
  in six segments, in a fabric material; cables and hoses are bezier
  curves with a bevel depth, converted to mesh (`new_from_object` on the
  evaluated object) and merged. Route cables clear of other parts, end
  them inside what they plug into, and keep them inside the footprint so
  a placement against a wall does not push them through it.

## Review before placing

- Render the kit's studio preview of each export (`vkr_asset_kit.preview.
  render_preview`: the glTF imported back into an empty scene, Cycles,
  interior studio light, front-left and back-right) and put the previews
  on one contact sheet; judge shape, wear and labels there before
  importing anything.
- After reimporting, check the level with `view.capture` in `base-color`
  and `normals` modes first: a model reimported without lightmap UVs draws
  dark under static lamps until it has them and a bake ran. Then capture
  the lit rooms and compare each model with its prototype's footprint; fix
  the model and `model.reimport` it rather than moving placements by hand.
