---
status: implemented
updated: 2026-10-10
authority: adr
---

# ADR-100: Lighting tools: bake settings, the time scrubber, outlines and the light list

## Status

Implemented. The editor sends bake settings with the project's Bake
lighting job and scrubs the time of day from the Lighting palette. It draws
the reach and shape of the selected lights and the boxes of selected
volumes. The Lights window lists every light and edits it in place. Agents
get the same controls through `lighting.*` operations.
[ADR-103](103-probe-and-volume-authoring.md) adds probe creation, outline
handles, the diffuse volume box and the Scene toolbar's time control.

## Context

[The artist toolkit proposal](../proposals/artist-toolkit.md) (Part 7,
Lights and bakes) found these gaps:
- Bakery accepts `lightmap_settings` and `diffuse_settings`, but the editor
  never sent them.
- The editor had no light gizmos and no light-group manager.
- The time of day ([ADR-090](090-time-of-day.md)) moved only through Cmd
  `time.hour` or scripts.
- Decals ([ADR-092](092-projected-decals.md)) and look volumes
  ([ADR-097](097-look-volumes.md)) showed no box in the viewport.

## Decision

1. **Bake settings.** [editor_lighting.c](../../editor/src/editor_lighting.c)
   describes both groups with type descriptors:
   - lightmap: `samples`, `max_depth`, `seed`, `page_size`,
     `texels_per_unit`, `direct_texels_per_unit` (the lamp-direct pages'
     density, "Lamp shadow density", [ADR-108](108-baked-lamp-direct-pages.md)),
     `denoise` (default, on or off), `denoise_iterations` and
     `indirect_clamp`;
   - diffuse volume: `spacing` (the finest probe spacing in metres),
     `face_size` (cube-face pixels per edge), `samples`, `max_depth`, `seed`,
     `photons` and `photon_radius`, the diffuse baker's options of those
     names.

   The project holds them for the session. The `bake_scene` request writes
   each group with only its nonzero values, so zero keeps Bakery's default.
   Cmd `scene.bake <samples>` still replaces the lightmap sample count. The
   Bake settings window (Cmd `window bake`, the Lighting palette's Bake
   settings) edits them as Details rows and runs Bake lighting. Without a
   project it says that baking needs one.
2. **Time of day.** The Lighting palette's TIME rows show the hour the
   scene renders and the World's starting hour.
   - A slider from 0 to 24 h moves the live clock, as Cmd `time.hour` does,
     until the simulation resets.
   - **Keep hour** writes the shown hour into the World's `time_of_day`
     component as an undoable edit.
   - Without an enabled World `time_of_day` the rows say how to add one.
3. **Outlines.** The selection's lights and volumes draw world-space lines
   in the overlay that collision shapes and level tools use (at most 192
   segments of its 512). None draw while scripts run.
   - Point light: three circles at its range; an unbounded light shows a
     small dim marker.
   - Spot light: the outer cone to its range (or 2 m), the inner cone
     dimmer, and its axis. The cone follows the entity's own rotation, as
     the lighting system aims it.
   - Rectangle light: its outline and an arrow along the emitting side,
     oriented through the hierarchy's rotations as the lighting system
     does.
   - Directional light: an arrow along its direction.
   - Look volume: its box and the box grown by its blend distance.
   - Decal: its box and an arrow along its projection (-Y).
   - Reflection probe: its world box (center and half extents) and its
     blend box.
4. **Lights window.** Cmd `window lights` and the Lighting palette's Light
   list open it.
   - **Light groups**: each group of the primary scene with a live
     intensity slider (0 to 4). Like Cmd `light.group`, it lasts until the
     simulation resets.
   - **Selected**: the selected light's Details rows, edited in place.
   - **Lights**: every light of the primary scene, then the World's, in
     entity order. Each row has an Enabled check box, the name (a click
     selects it), Static or Dynamic (a click switches it), and the group
     and intensity. Edits are undoable. Only the rows in view are built.
5. **Operations.**
   - `lighting.list`: the lights with their kind (`directional`, `point`,
     `spot`, `rectangle`), `from` (`scene` or `world`) and component
     values, and the scene's groups with their intensity and current
     factor.
   - `lighting.group`: `group` and `intensity`, live.
   - `lighting.time`: `hour`, live; keeping an hour is a `time_of_day`
     component edit.
   - `lighting.bake`: `lightmap` and the two settings groups. The values
     are read through the type descriptors, so an unknown key or an invalid
     value fails. They become the window's values. The job then starts.

## Consequences

- One action previews a lighting change (hour, group level) without editing
  the level. Keeping it is a separate, undoable edit.
- Bake quality is set per session; a project does not store it yet.
- A dynamic light belongs to no group ([ADR-090](090-time-of-day.md)), so
  the group list shows only static lights' groups.
- Outlines show the selection only, so a level with hundreds of lights stays
  readable.

## Evidence

On a headless Bistro editor (Release, Metal), on 2026-10-09
(`.scratch/artist/phase5d_lights.py` and `phase5d_bake.py` in the working
tree):
- **List.** `lighting.list` returned Bistro's 74 lights and its `default`
  group. It also returned the created point, spot and rectangle lights with
  their kind, mobility, group and range.
- **Outlines.** Captures with each object selected showed:
  - the 3 m point light's three circles;
  - the spot light's two cones and axis;
  - the rectangle light's outline and arrow;
  - the decal's box and arrow;
  - the look volume's box and blend box;
  - Bistro's indoor reflection probe's box and blend box.
- **Groups.** `lighting.group lamps 0` registered the group with
  intensity 0 and factor 0. A negative intensity and an invalid name were
  refused.
- **Windows.** The Lights window showed the groups, the selected light's
  rows and the list rows. The Bake settings window showed its note without
  a project.
- **Bake.** On a project scene (FPS Arena), `lighting.bake` with lightmap
  samples 16, denoise off, voxel size 0.5, 64 samples and 20,000 photons
  wrote both groups into the request. The diffuse job ran with
  `--voxel-size 0.5`, `--samples 64` and `--photons 20000`. An unknown
  settings key was refused. `lighting.time 18.5` moved the clock.
- **Not run:** no CPU test was added. The settings go through type
  descriptors, which the scene document tests cover, and the editor code
  has no CPU suite. Timing was not measured: the outlines exist only while
  something is selected, and the window only while open.

## Alternatives considered

- **Outlines for every light at once.** Rejected: Bistro's 74 lights would
  fill the 512-segment overlay with circles.
- **Bake settings in the project file.** Deferred: the project format has no
  bake section yet, and the session values cover iteration.

## Revisit when

- Projects store bake settings.
