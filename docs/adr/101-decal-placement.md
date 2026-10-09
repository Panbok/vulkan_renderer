---
status: implemented
updated: 2026-10-09
authority: adr
---

# ADR-101: Decal placement on surfaces

## Status

Implemented. The Art workbench's Decal tool places a decal where a click
meets a surface, facing that surface. `decal.place` does the same for
agents, at a point with its normal or where a ray meets collision. Both
share one orientation rule.

## Context

A `decal` component ([ADR-092](092-projected-decals.md)) projects along its
box's -Y. Before this decision an artist created one at the Scene's center
from Create or the Lighting palette's object list. They then turned it by
hand so that +Y faced the surface. Agents had only `entity.create` with a
rotation they had to work out. The [artist toolkit
proposal](../proposals/artist-toolkit.md) (Part 8, Dressing) asks for a
click-to-place decal tool.

## Decision

1. **Orientation.** `vkr_editor_decal_orientation` (normal, facing, angle)
   in [editor_viewport.c](../../editor/src/editor_viewport.c) turns the
   unit box so that:
   - +Y lies along the surface normal;
   - local +Z, the image's top in the tiled shader's decal UV
     (`v = 0.5 - z`), points up a wall;
   - on a floor or ceiling (|normal.y| ≥ 0.9), local +Z points along
     `facing` projected onto the surface, else along -Z;
   - the result is then turned by `angle` about the normal.
2. **Box.** The box is centred on the surface point. Its scale is the width
   and height across the surface (X and Z) and the depth along the normal
   (Y), so it reaches half the depth to either side.
3. **Tool.** Cmd `decal.tool` and the Art palette's DRESSING Decal button
   start the Decal Scene tool. It excludes the other Scene tools and face
   handles.
   - The surface under the pointer (collision, else the grid plane, as the
     measure tool finds it) shows the box a click would place, with a tick
     toward the image's top.
   - A click sends `decal.place` through the agent channel with the
     pointer ray as `facing`. The new decal is selected, and the edit is
     undoable.
   - The palette sets Size (0.1 to 8 m), Depth (0.05 to 4 m) and Turn (0 to
     360 degrees) for the session; the defaults are 1 m, 0.5 m and 0.
   - The decal takes the Material panel's open material file, else the
     default decal.
   - Escape stops the tool.
4. **Operation.** `decal.place` takes either `position` and `normal`, or
   `origin` and `direction`. With a ray, it places the decal where the ray
   first meets collision within 1000 m, with the ray as the default
   `facing`. It also takes these optional arguments:
   - `size`: one number, or [width, height]; default 1;
   - `depth`: default 0.5;
   - `angle`, in degrees;
   - `facing`;
   - `material`;
   - `values`: other decal fields;
   - `name`, `container`, `review` and `select`.
5. **Scene image.** `editor.status` reports `view.image`, the Scene image's
   [x, y, width, height] in window points. Scripts can then aim `ui.click`
   into the Scene.

## Consequences

- A decal lands facing the surface in one click, and an agent places one
  from a raycast without computing a rotation.
- A wall decal's image stays upright whichever way the wall faces. A floor
  decal faces the viewer who placed it.
- Without collision under the pointer the tool falls back to the grid
  plane. Bistro's imported meshes have no collision, so on Bistro the tool
  places on the grid plane or on brushes.

## Evidence

On a headless Bistro editor (Release, Metal), on 2026-10-09
(`.scratch/artist/phase6a_decal.py` in the working tree):
- **Point.** `decal.place` at a street point with normal +Y and size 2 drew
  the default decal on the street.
- **Ray.** A brush wall was added for collision, because Bistro's meshes
  have none. A ray from the camera met it at x = -4.2 with normal -X.
  `decal.place` with that ray, size [1.5, 1] and angle 15 created
  `decal/Wall` there. Its scale was (1.5, 0.5, 1), and its outline lay on
  the wall, turned 15 degrees.
- **Refusals.** These failed with their reasons:
  - a ray that meets nothing;
  - a `position` without a `normal`;
  - a negative size.
- **Tool.** After `decal.tool on`, `ui.click` inside `view.image` created and
  selected a `Decal`. Moving the pointer showed the preview box on the
  surface.
- **Not verified:** the image's upright orientation on a wall with an
  asymmetric decal image (the default decal is round). No CPU test was
  added. The orientation is three projections and a look-at, and the
  captures check the placement.

## Alternatives considered

- **The placement tool's tilt (`place_orientation`).** Rejected: the
  shortest rotation from +Y to a wall's normal leaves the image's top along
  the wall on one facing and along the floor on another.
- **A decal tool owned by a new module.** Rejected: the Scene tools, their
  ray and surface helpers and their overlay already live in
  `editor_viewport.c` and `editor_physics.c`.

## Revisit when

- Decals need drag handles to resize them in place.
- Bistro's meshes get collision, or the tool should pick GPU depth where
  collision is missing.
