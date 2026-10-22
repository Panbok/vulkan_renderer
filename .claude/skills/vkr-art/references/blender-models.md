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
  offset.
- Build from bevelled boxes, frustums and cylinders through `bmesh`; merge
  doubles, smooth by angle (about 35 degrees) and apply a Weighted Normal
  modifier so bevelled edges catch light without a normal map.
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
- Give every face of a plain material white (1, 1, 1, 1): a plain material
  multiplies its base colour by COLOR_0.
- Remove other colour attributes, make the attribute active and the render
  colour, and export with `export_vertex_color="ACTIVE"`. Check the glTF:
  a VEC4 COLOR_0 of 16-bit normalized values, read with the accessor's
  component type.

## Review before placing

- Render a Workbench preview of each asset (material colour, cavity,
  studio light) and put the previews on one contact sheet; judge shape and
  proportion there before importing anything.
- After placing, capture the level's rooms with `view.capture` and compare
  each model with its prototype's footprint; fix the model and
  `model.reimport` it rather than moving placements by hand.
