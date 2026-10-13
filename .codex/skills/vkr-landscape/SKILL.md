---
name: vkr-landscape
description: Generate, sculpt, paint and dress VKR terrains through the editor's agent channel - eroded heightmaps, stamps and weight paint, holes and terrain_cutout volumes for rooms set into ground, backdrop terrains, rivers and lakes, forest and rock scatters, the sun's hour - and check their look and frame cost. Use for landscape work; use vkr-level-design for brush spaces, gameplay pieces and the channel's basics.
---

# VKR landscapes through the agent channel

[ADR-084](../../../docs/adr/084-agent-channel-and-level-design-toolkit.md)
defines the terrain, scatter and cutout operations; `ops.list` returns their
schemas. Connect, claim and verify as
[vkr-level-design](../vkr-level-design/SKILL.md) describes; this skill sets
how a landscape is made and proved.

## Plan around the level

- Fix the structures first: every portal, station, bridge deck, buried room
  and path with its floor height and the clearance it needs. The terrain is
  then shaped to them, not the other way round.
- Pick the size, spacing and stored height range at `terrain.create`;
  changing them later resamples everything. A 2,048 m playable terrain at
  2 m spacing is 1,025 samples a side. Leave the range 220 m or more above
  the highest peak you want: the generator rolls peaks off under it instead
  of flattening them into plateaus.
- Write the constraints as boxes for the generator: ground at least a
  height over a buried room (`--floor`), at most a height through a
  corridor or under a bridge (`--cap`), benches at portals. A cap carves a
  cone of slope 0.9 around its box, so a cap 100 m under the ground opens a
  crater about 110 m wider on every side; keep caps shallow or cut the
  rest with a cutout.

## Generate

`scripts/alpine_terrain.c` writes a playable terrain and its backdrop from
one height function: domain-warped ridged massifs, an optional U valley
with a cliff, terrace and upper slopes, droplet and thermal erosion, the
valley carved again after erosion (sediment fills it), the constraint boxes,
and layer weights. Its printed `center` y assumes a terrain entity at y 0;
add the entity's y otherwise. Copy it to `.scratch/`, change its world function for
the level, then:

```sh
cc -O2 -o alpine_terrain alpine_terrain.c -lz -lm
./alpine_terrain --out DIR --center X Z --size 2048 --min -160 --max 1040 \
    --valley X --valley-floor -40 --floor X0 Z0 X1 Z1 H --cap X0 Z0 X1 Z1 H
```

It runs in about 10 s and 50 MB, and prints the stamp `center`, `size` and
`height_scale` for both grids. Read `main_preview.png` before stamping: a
smooth, uneroded or terraced relief, or a crater around a cap, is fixed in
the generator, not by sculpting. Then find closed depressions:

```sh
python3 depressions.py --heights DIR/main_h.f32 --n 1025 --origin X0 Z0 --step 2
```

A deep, small region near an edge is an outlet pit: droplets leaving the
grid carry their sediment off, so a valley draining over an edge digs its
outlet (40 m deep at the Black Mesa Alps' south end before the generator
limited erosion depth). A broad, shallow region on a valley floor is a lake
basin, and its spill level is the highest water it holds.

## Apply and check

1. Stamp heights: `terrain.stamp` `mode` set with the 16-bit PNG, `center` y
   the terrain entity's y plus its `height_min`, `height_scale` the range.
   Give `image` an absolute path; a relative one resolves against the
   scene's asset root. One pixel per sample, with the stamp's min corner
   (`center` less `size` / 2) on a sample, stamps exactly; anything else
   resamples.
2. Paint weights: `mode` paint with the RGBA PNG; red to alpha weigh layers
   1 to 4, which are `layer0` to `layer3` (grass, rock, scree, snow from
   the generator). Weigh by slope, height above the valley floor, facing
   (snow lower on north faces) and settled sediment (scree on talus).
3. Prove each constraint with `terrain.sample` along cross-sections through
   every portal, deck and room: ground under each floor where it should
   carry, clearance where it should not. A preview does not show a 2 m
   bench missed by 3 m. A `region` sample answers `grid` `rows` (4,096
   heights a call), the cheapest way to read an area back for a script.

## Rooms, portals and cliffs

- Put a `terrain_cutout` box around every room set into the ground (Create
  menu Level > Terrain Cutout, or an entity with the component). The ground
  inside the box, within its height range widened by `margin`, draws and
  collides as a hole without changing a sample, so moving or deleting the
  box restores it. The opening follows the sample grid; the room's shell
  must cover a spacing past the box. A streamed terrain (over 1,024 cells
  a side) draws far tiles from overview cells that ignore cutouts, so its
  cut shows ground from kilometres away.
- Keep sculpts clear of finished rooms: `terrain.ramp` and
  `terrain.flatten` blend into the ground over `falloff` metres beyond
  their footprint, and that blend raises or lowers ground through a nearby
  room (a view cut beside the Black Mesa gallery filled its far end). Check
  footprint plus falloff against every room, or use a cutout instead. To
  undo a sculpt, crop the generator's floats over the damage with
  `scripts/restore_crop.py` and stamp them back:

  ```sh
  python3 restore_crop.py --heights DIR/main_h.f32 --n 1025 \
      --origin X0 Z0 --step 2 --range MIN MAX --rect X0 Z0 X1 Z1 \
      --out /abs/restore.png --terrain NAME
  ```

  It prints the `terrain.stamp` call that writes those samples back
  exactly (heights only; paint stays). The crop is a square from the
  rectangle's min corner, so a long rectangle restores ground past its
  short side too; `--range` is the terrain's local `height_min`/`height_max`,
  and `--terrain-y` adds the entity's y.
- Terrain UVs project straight down, so faces over about 65 degrees stretch.
  Keep visible walls under that, and set a portal or window in a cliff as a
  concrete block in the slope with a `terrain.hole` (or cutout) on grid
  lines behind it; the block covers the cut's edges.

## Backdrop

The far edge of a terrain reads as a void. Surround the playable terrain
with a coarse backdrop terrain (16 m spacing from the same generator). Inside
the playable footprint it must lie at least 8 m under the playable surface
one backdrop sample in, and equal it along the border; a backdrop only a
metre under z-fights at distance, where depth precision is coarse. The
generator writes both that way. Set the backdrop terrain's `backdrop`
flag: it then draws and collides only outside the playable terrain's
footprint, so the sunk surround never crosses rooms and tunnels dug under
the playable ground (it cut through a buried transit tunnel 50 m down
before). `view.camera` `far` sets how far captures see.

## Rivers and lakes

Water is `water`-surface `visual` brushes over a bed shaped by a script
that reads the area back, reshapes it and stamps it in square crops.

- A river's level falls along its flow and never rises; put lakes in the
  basins `depressions.py` finds, at or under their spill level. Fit the
  level to the smoothed bed, then carve the channel and deepen the lake
  from it; a level set from raw samples digs gorges through every bump.
- Shape the shore from a smoothed outline, not from the samples under the
  level, which follow the floor's noise into a spiky shore.
- Keep every sample at least 0.25 m under or 0.15 m over the water: ground
  within millimetres of the water plane z-fights with it across the whole
  flat.
- Build the water as convex hulls between grid rows (`brush.hull`, eight
  points), each covering every wet row between its two rows, so neighbours
  share edges and never overlap. Raise any ground a hull covers beyond its
  rows' water above it, or the hull's corner shows as a square notch.
  End the water where the ground rises over it, not at a terrain border.
- Erase scatter areas near the water before judging it (`scatter.paint`
  `erase` deletes areas whose centre lies within `radius`; it answers an
  error when no area lies there).

## Foliage and rocks

- Scatter cooked glTF props with one submesh each: a cooked OBJ has no mesh
  nodes and places nothing, and every extra submesh is another draw per
  copy. Bake bark and needles into one atlas material per tree.
- A scene holds 64 rules (scatters and spline meshes), 2,048 copies a rule
  and 12,288 copies in all (each copy is a mesh-manager instance). Split a
  species over rules of about 1,900 copies so a rule never clips.
- Choose candidates by the generator's data, not by look: species bands by
  height above the valley floor (fir low, spruce, larch toward the tree
  line, shrubs above), slope limits, a clumping noise for stands, boulders
  on talus and river banks. Rank them by distance to the player's views so
  the budget lands where it is seen, and keep paths, decks, portals and
  cutouts clear.
- Every copy is a culling candidate for every view, shadow cascades and
  local shadow faces included. Count the views before adding thousands.
- Copies leave the camera below 2 pixels across and cast sun shadows only
  into cascades that start within 50 m. To change that for a rule,
  `draw_distance.set` its scatter or spline entity (see
  `vkr-level-design`, Draw distance); every copy takes the rule's values.

## Light and look

- Light the mood with the hour: a low afternoon sun (World `time_of_day`
  `hour`, tried live with `lighting.time`) shades valley floors and lights
  the peaks; noon flattens a valley. The World's hour applies to every scene
  of the project, so name the change to the designer.
- Bakes leave terrain out; it is lit at runtime. With an atmosphere the bake
  adds eight sun-key layers beside each lamp group's (420 MB in the Black
  Mesa level). Set the
  `diffuse_settings` `bounds_min`/`bounds_max` (Lighting, Box min/max)
  around the playable spaces, or far terrain props widen the diffuse volume
  past its brick budget and the bake drops it.
- Judge from player positions at eye height with the grid off and `far`
  past the backdrop; an overhead capture hides seams, stretch and empty
  slopes.

## Frame cost

Measure before blaming the terrain. Read Cmd `stats.frame_ms_p95` at fixed
views while hiding one suspect at a time (forest scatters, backdrop,
clouds, atmosphere), and read pass timings with `window draws on`. Known
costs:

- A shadowed lamp whose range holds a mover or a physics body renders its
  shadow faces every frame, and each face adds a culling view over every
  scatter copy. Turn off `casts_shadow` on lamps near moving bodies, or
  move the bodies away.
- Per-pixel opaque, sky and cloud cost scales with the window's pixels:
  compare frame times at one window size, and expect a Retina window to
  cost more than a headless capture.
- Headless numbers set the direction; a timing claim needs the matched
  Release Bistro reports that `vkr-performance` describes.

## Done when

The constraint cross-sections hold, captures from the player's views show
no void, seam, stretch or empty slope, rooms set into the ground are clear
to walk, the scatter counts match the plan without clipping, the frame cost
was measured with each new layer hidden and shown, and the scene is saved.
