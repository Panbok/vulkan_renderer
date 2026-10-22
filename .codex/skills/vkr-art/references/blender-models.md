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

## Materials and UVs

- Give each surface its own material slot named after the library material
  it will take (`paint_blue`, `steel`, `wood_desk`, `plastic_dark`,
  `screen`). The engine's slots follow the glTF's primitive order, which
  follows the slot order. Keep to eight slots: a ninth cannot take a
  library material.
- Box-project UVs in metres: a face's two widest axes divided by the
  material's tile size. Library materials then tile at their `world_size`
  without per-model unwrapping.
- Build each Principled material as the library texture times a tint: an
  image node into a MixRGB Multiply node exports as a texture with a
  `baseColorFactor`. Split the ORM texture so G drives Roughness and B
  Metallic; the exporter writes them as one `metallicRoughnessTexture`.
  Turn on backface culling; it exports single-sided.
- Faces that show an image (screens, signs) need 0..1 UVs: give them a
  material of their own (`display`) and normalize each face's projected
  coordinates to its bounds when building UVs, U along the face's width as
  seen from the front and V up. A second screen material (`display_b`)
  lets one model show two images. Image textures of 16:9 suit a display's
  glass; draw them with a script (dark UI, cyan and orange accents) and
  cook them like any texture.

## Wear in vertex colours

- A layered material with the `vertex_color` mask reads COLOR_0 as weights
  of layers 0 to 3. Write a corner (face corner) float colour attribute:
  R paint, G chipped bare metal, B grime, A unused. Keep R at least 0.05 so
  the weights never sum to zero.
- Put chips on bevel strips only (faces whose shortest edge is under about
  9 mm), scaled by a cheap position noise; put grime on corners below
  0.5 m, falling to zero at that height.
- A plain material multiplies its base colour by COLOR_0, so its faces
  carry baked ambient occlusion there (next section), or white
  (1, 1, 1, 1) where they should stay unchanged.
- Remove other colour attributes, make the attribute active and the render
  colour, and export with `export_vertex_color="ACTIVE"`. Check the glTF:
  a VEC4 COLOR_0 of 16-bit normalized values, read with the accessor's
  component type.

## Ambient occlusion, labels and soft parts

- Bake ambient occlusion into the plain faces' COLOR_0 with Cycles:
  `bpy.ops.object.bake(type="AO", target="VERTEX_COLORS")` into a
  temporary corner attribute, with a large plane just under the origin so
  feet and plinths darken where they meet the floor, and the world's AO
  distance about 0.3 m. Store `floor + (1 - floor) * ao` with a floor near
  0.4, keep layered faces' weights, and keep emissive, screen and label
  faces white. Remove the temporary attribute before export. The bake
  takes well under a second per prop. Lightmaps still add the room's
  occlusion; the vertex term adds the contact shadows a lightmap texel is
  too coarse for.
- Printed stickers and plates come from one atlas texture of square cells
  (logo, hazard signs, barcodes, rack and room plates, serial plates): a
  thin box of a `labels` material per sticker, a face integer attribute
  naming its cell, and UVs normalized over each face into that cell with
  a 2% inset. Make the sticker's aspect match the cell's, stand it 1 to
  2 mm proud of the surface, and give the material a semi-gloss plain
  definition with the atlas as `base_color_texture`.
- Look up `bmesh` custom layers by name after every merge
  (`bm.faces.layers.int["cell"]`): `from_mesh` can add layers and a handle
  kept from before then reads another layer, so labels added before a
  cable showed the wrong cell.
- Figures and displays inside enclosures: leave the front open or keep
  glass to a thin band, because tinted glass hides what the enclosure is
  for (an HEV suit in its pod). `Builder.sphere` makes helmets and domes.
- Soft furnishings are boxes bevelled by 40 to 50% of their smallest size
  in six segments, in a fabric material; cables and hoses are bezier
  curves with a bevel depth, converted to mesh (`new_from_object` on the
  evaluated object) and merged. Route cables clear of other parts, end
  them inside what they plug into, and keep them inside the footprint so
  a placement against a wall does not push them through it.

## Review before placing

- Render a Workbench preview of each asset (material colour, cavity,
  studio light) and put the previews on one contact sheet; judge shape and
  proportion there before importing anything. Material colour shows labels
  and screens blank: render a second sheet with `color_type` `TEXTURE` to
  check every label's cell and orientation. Render with backface culling on
  (`BM_PREVIEW_CULL=1` in `preview`) to catch flipped faces, which the
  engine culls.
- After placing, capture the level's rooms with `view.capture` and compare
  each model with its prototype's footprint; fix the model and
  `model.reimport` it rather than moving placements by hand.
