---
status: implemented
updated: 2026-10-09
authority: adr
---

# ADR-102: Scatter painting with painted areas

## Status

Implemented. A brush paints a scatter's copies onto surfaces. Each dab
becomes a `scatter_area` child of the scatter, which fills a disc with
seeded copies. The Art workbench's Scatter paint tool and `scatter.paint`
add areas, or erase the areas under a stroke. A stroke is one undo step.

## Context

A `scatter` ([ADR-084](084-agent-channel-and-level-design-toolkit.md))
drops `count` seeded copies in its box. The scene rebuilds them from the
rule and never saves them. Dressing a street needed copies where an artist
points, not spread over a box. [The artist toolkit
proposal](../proposals/artist-toolkit.md) (Part 8) asks for a brush that
paints scatter rules onto surfaces, with a density and a radius, within the
4,096-copy bound.

Components have no array fields, so painted data needed a representation.
On 2026-10-09 the owner chose painted areas from three options:
- painted areas;
- one entity per painted copy;
- a density mask file.

## Decision

1. **Areas.** A `scatter_area` component
   ([vkr_scene_system.h](../../runtime/src/renderer/systems/vkr_scene_system.h))
   on a child of a scatter holds these fields:
   - `radius` (0.05 to 100 m);
   - `density` (copies per square metre);
   - `spacing` (metres);
   - `seed`;
   - `order`.

   The area's centre is its entity's world position. Area entities are
   saved like spline points, show the scatter's icon in the Outliner and
   have no Scene label.
2. **Placement.** A scatter with at least one area places copies only in
   its areas, not in its box
   ([vkr_scene_population.c](../../runtime/src/renderer/systems/vkr_scene_population.c)).
   - Areas run in ascending `order`, ties in child order.
   - Each area asks for density × πr² copies. The fractional part is kept
     by chance, so a small area still places some.
   - Each copy has a uniform point in the disc and drops straight down from
     a radius plus 1 m above the centre to as far below. It lands on the
     first physics surface, or on terrain samples above it, as box copies
     do.
   - The rule's `align_to_surface`, `random_yaw`, scale range and rotation
     apply. The streams use the area's own seed.
   - A copy within the area's `spacing` of an earlier copy of the scatter
     is left out. A later area therefore never moves earlier copies, and an
     undo that brings areas back keeps their `order`.
   - The rule's signature hashes each area's values and world position, so
     painting, moving and erasing rebuild it.
   - The per-rule bound (2,048 copies) and the scene bound (4,096) still
     cap the total. The status reports copies that found no ground, or no
     room for their spacing.
3. **Operation.** `scatter.paint` takes these arguments:
   - `scatter`: an existing entity with a `scatter`;
   - `points`: 1 to 256 world positions;
   - `radius`, `density` and `spacing`.

   It adds one `Area` child per point, positioned in the scatter's space.
   - Each child's `order` follows the scatter's highest. Its `seed` hashes
     the point and the area's index, so the same stroke lands the same way.
   - With `erase` it deletes instead the areas whose centre lies within
     `radius` of a point, across the ground (x and z). It fails when none
     does.
   - Either way the stroke is one batch: one undo step and one review.

   `entity.get` reports a spline mesh's or scatter's placed `copies` and
   `status`.
4. **Tool.** Cmd `scatter.tool` and the Art palette's DRESSING Scatter paint
   button start the Scatter paint Scene tool. It paints the selected
   scatter, or the scatter of a selected area.
   - While the button is held, the tool lays a dab every half radius
     across surfaces under the pointer (collision, else the grid plane). It
     holds up to 128 dabs.
   - The release sends one `scatter.paint`. Shift at the press erases.
   - The overlay shows the brush ring (green, or red while erasing), the
     stroke's dabs and the scatter's existing areas.
   - The palette sets Radius (0.25 to 20 m), Density (0.01 to 10 per square
     metre) and Spacing (0 to 5 m) for the session. The defaults are 2 m,
     0.5 and 0.5 m.
   - Escape drops a stroke, then stops the tool.

## Consequences

- Painting adds tens to hundreds of small entities per scatter. Documents
  stay small, and copies stay generated.
- Copies cannot be moved one by one. Erasing removes whole areas, and the
  copies of overlapping areas go with them.
- Overlapping dabs add density. Spacing thins them, in order.
- A painted scatter still needs collision or terrain below. Bistro's
  imported meshes have none, so painting there needs a brush or terrain
  surface.

## Evidence

On a headless Bistro editor (Release, Metal), on 2026-10-09
(`.scratch/artist/phase6b_scatter.py` in the working tree). A brush slab
under the street provided collision, and the scatter placed Bistro's
`bistro-lights-main.vkb` mesh 246 at scale 22 to 30.
- **Box, then areas.** The box scatter placed its 40 copies.
  - Stroke A (6 points, radius 1.2 m, density 1.5, spacing 0.4 m) replaced
    them with 27 of the 41 copies its areas asked for.
  - Stroke B (4 points elsewhere, radius 1 m, density 2, spacing 0.3 m)
    brought 46 of 66.
  - The capture shows the copies on the street.
- **Stability.** Pixels that changed by more than 24 levels:
  - between two captures after stroke A: the frame-time label only;
  - between the captures after A and after B: B's part of the street and
    the label. A's rows of the image did not change.
- **Erase and undo.**
  - Erasing two of B's dabs deleted three areas (10 to 7) and left 33
    copies.
  - `undo` brought back 10 areas and exactly the 46 copies.
  - An erase where no area lies failed with its reason.
- **Refusals.** These failed with their reasons:
  - empty `points`;
  - an unknown scatter;
  - a negative radius.
- **Bound.** One area of radius 4 m at density 50 (about 2,513 copies)
  placed 2,048, with the status "Capped at 2048 copies by the scene's
  population bound". No area tries more columns than the rule may hold
  (2,048), so a huge density costs a bounded number of raycasts.
- **Tool.** With the scatter selected, `scatter.tool on` and a `ui.drag`
  across the Scene added one stroke of 6 areas. The overlay showed the
  rings.
- **CPU suites.** `vulkan_renderer_tester` passed its 98 other suites with
  the new type registered. `run_local_socket_tests` was left out of that
  run: its `stale_and_live` case fails on this host, with and without the
  sandbox.
- **Not run:** a save and reload of a painted scene. Areas use only number
  properties and a parent, which the scene document tests cover. Timing was
  not measured. Painting changes copies only when a stroke lands, and the
  copies draw as the existing population instances do.

## Alternatives considered

- **One entity per painted copy.** Rejected (owner, 2026-10-09): up to
  2,048 entities per scatter make documents and the Outliner heavy.
- **A density mask file.** Rejected (owner, 2026-10-09): a new document type
  and file I/O, and copies that reshuffle whenever the mask changes.
- **Areas in child order.** Rejected: undoing an erase brings areas back
  last, and spacing then kept a different set of copies (69 instead of 68
  in the first run).

## Revisit when

- Artists need to move or delete single copies.
- Scatters need a mask or slope limits per area.
- Painting should erase copies rather than whole areas.
