---
status: partial
updated: 2026-10-13
authority: adr
---

# ADR-115: Rooms and portals

## Status

Accepted (partial). Rooms hide the camera's draws, lights and reflection
probes that no open portal shows, and sealed rooms keep their draws out of
every cascade, on both pipeline classes; doors shut their portals,
`room.probes` gives rooms their probes and the `rooms` lint check finds
leaks. `room.derive` and linting objects no room holds from the
[large-level rendering](../proposals/large-level-rendering.md) proposal are
not built. The tiled pipeline has native Metal evidence; the desktop
pipeline has compiled SPIR-V only.

## Context

Inside an indoor base the camera sees one to three rooms, but every
candidate of the level was classified, encoded and drawn when it passed the
frustum. The HZB ([ADR-028](028-gpu-driven-deferred-visibility-buffer.md))
rejects nothing while the camera moves, and two-phase occlusion was declined
([ADR-032](032-two-phase-confirmed-visibility.md)). Terrain, backdrop and
forests above a base passed the frustum whenever the camera looked up or
along a corridor.

## Decision

1. **Authoring.** A `room` component (`SceneRoom`,
   [vkr_scene_system.h](../../runtime/src/renderer/systems/vkr_scene_system.h))
   places a box, [-0.5, 0.5] on each axis of its entity's world transform,
   around a room's interior; a room entity whose parent is a room adds its
   box to it, for L-shaped spaces. `sealed` keeps sunlight out. A `portal`
   component places a rectangle, [-0.5, 0.5] in X and Y facing Z, in an
   opening; `open` false hides what lies behind it. `blockout.room` adds a
   room box on the room's interior (and takes `sealed`); `blockout.doorway`
   adds an open portal in the opening.
   An open portal's `door` names a mover
   ([ADR-084](084-agent-channel-and-level-design-toolkit.md)); `mover.create` with `portal`
   sets it.
2. **Graph.** [vkr_scene_rooms.c](../../runtime/src/renderer/systems/vkr_scene_rooms.c)
   rebuilds the graph when the scene's world or structure revision changes
   or a room or portal moves: up to 1,024 rooms of 4,096 boxes and 2,048
   portals. A portal joins the rooms found 0.3 m to either side of it, a
   room or the outside, the space no room holds. A scene without a room
   searches for one once per structure revision.
3. **Membership.** Each static draw, per submesh, belongs to up to two
   entries of the rooms within 0.5 m of its bounds and the outside, which it
   touches when a corner or the centre of its box lies in no room. A draw
   touching more is unassigned and never hidden. Terrains and scatter copies
   belong to the outside; moving draws are unassigned. The table rebuilds
   when the graph or the mesh manager's topology, static content or
   population changes. Room boxes therefore hold interiors only: a box that
   covers a wall would take the wall's outside face with it.
4. **Walk.** Each frame the camera's room, or the outside, sees every room
   reachable through open portals inside the view, each portal narrowing the
   screen rectangle to its projected corners (a portal crossing the near
   plane, or around a camera in it, keeps the whole rectangle). A node is
   walked again only through a rectangle wider than any it was walked with,
   so cycles end. A portal whose door rests at its saved pose, the closed
   one, counts as closed: the door has no evaluated pose (movers move only
   in play), or its pose and the pose it draws from both equal its parent's
   world times its local transform within 1 mm. Door poses are read each
   frame and change no revision, so a moving door rebuilds no graph or
   membership. The graph resolves the door once per rebuild and ignores
   one that is no mover, which would never open.
5. **Candidate bits.** The emission ORs two bits into the cull word of
   [ADR-114](114-per-object-draw-distance.md): bit 20 when all of a draw's
   entries are hidden this frame, bit 21 when all are sealed rooms. A change
   of the static candidates' bits repacks the backends' resident static rows
   through `static_cull_generation` (ADR-114). The
   camera test in `lod_kernel.slangh` drops a hidden candidate first; the
   cascade test drops a sealed one in every cascade (`shadow_near` is at
   least 1 mm for cascades, zero for other views). Local faces keep every
   caster. The CPU blend list applies the hidden bit too. A change of what
   the sealed rooms keep out notes one caster change, so retained shadows
   redraw once.
6. **Lights.** Before the scene syncs, the runtime walks the rooms for the
   camera and gives the lighting system a gate
   (`VkrLightingSystem.point_light_hidden`, `vkr_scene_rooms_light_hidden`):
   a point or spot light, or a baked lamp offered for its moving casters'
   shadows, leaves the frame's candidates when its room, or the outside,
   is hidden and its range sphere reaches no visible room's box nor, while
   the outside shows, past its own room's box. Unshadowed light passes
   through walls, so any reach keeps it. A light without a range is never
   hidden. Its cached local faces stay, as for a light that leaves the
   table by distance (ADR-019).
7. **Probes.** A reflection probe whose box and blend distance hide by the
   same rule leaves the frame's 16 slots to the probes the camera can see
   (`vkr_scene_rooms_box_hidden`). A draw in a room keeps taking the
   smallest probe box around its centre, so a probe on a room's box wins
   over the hall around it. `room.probes` adds a box-projected probe on the
   box of each room that holds none, up to the scene's 16 probes.
8. **Lint.** `vkr_scene_rooms_check` reports a portal with one space on
   both sides, a portal whose door names no mover, and a gap in a room box's face: samples 0.5 m apart, 0.1 m
   past the face, that no solid (the editor's collision), other room box or
   portal opening closes, kept when their area reaches 0.25 m².
   `level.lint` reports them as `rooms`
   ([editor_level.c](../../editor/src/editor_level.c)) with the portal or
   room box and the open area. Box containment takes its tolerance in
   metres, so a long box does not swallow a nearby sample.
9. **Switches.** A probe capture, the editor Scene's "All" and the
   `VKR_ROOMS=0` diagnostic draw every room, light and probe.

## Consequences

- Rooms hide only what portals do not show, so captures with rooms on and
  off match when the portals cover every opening. On the large-level
  fixture (Metal, `local-offscreen`, 2026-10-13), the café bar view, where
  the walk hides the outside, matched `VKR_ROOMS=0` exactly. The café west
  view matched a `VKR_ROOMS=0` run exactly; Bistro alternates between two
  outputs near a wall sconce from run to run (0.12% of pixels), with rooms
  on or off.
- On the café walk (`large_level_bistro_metal_cafe_walk`,
  `local-offscreen-gpu-single`, one non-authoritative run each) the
  camera's visible rows fell from a median of 3,433 to 2,741 and a mean of
  3,359 to 2,658; `Tiled.Opaque` stayed at 4.6 ms, since the hidden draws
  covered no pixels. Indicative, not a timing claim.
- With the light and probe gates (2026-10-13), the café bar view matched
  `VKR_ROOMS=0` within one level of 255 and the overview's depth matched
  exactly; its colour moved 1% of pixels by up to 21 levels, all in the sky,
  through the automatic exposure the warm-up frames feed.
- `blockout.room` and `blockout.doorway` made a room box on the interior and
  a portal in the opening on Bistro through the agent channel, and
  `level.lint` with `kinds` [`rooms`] then reported nothing. With the
  portal deleted it reported the doorway's 2 m² gap, and a stray portal in
  open space as joining nothing.
  `mover.create` with `portal` set the portal's door to the new mover.
  The room suite's door test (`test_room_portal_door`,
  [room_test.c](../../tests/src/room_test.c)) checks the walk hides the
  room behind a resting door, shows it with the door posed 2.5 m up and
  again hides it at rest under an evaluated pose; no level with movers
  between rooms has been captured.
  `room.probes` was run only on a scene outside a project, where it refuses
  as `probe.create` does; its probe creation in a project scene is not
  exercised.
- Metal API validation of the café bar and overview captures
  (`local-metal-offscreen-validation-serial`, `MTL_DEBUG_LAYER=1`) reported
  no error.
- A missing portal hides visible content; a missing room leaves its draws on
  the outside. Authoring errors show as holes, not as cost.
- The membership rule over-assigns: a wall between two rooms usually also
  touches the outside and becomes unassigned. Props, the bulk of draws,
  belong to their room.
- No GPU buffer or binding is added: the CPU resolves the bits per candidate
  in the emission it already runs.

## Alternatives considered

- **A GPU visible-room bitset and per-candidate room indices** (the
  proposal): rejected, since candidates are emitted every frame and the CPU
  can resolve the bit there without a binding or side array.
- **A precomputed PVS per cell.** Rejected: every brush edit invalidates
  the bake and doors must be assumed open.
- **Rooms derived from brushes.** Kept for an editor aid (`room.derive`):
  they leak through gaps and fit open halls poorly.
- **Two-phase occlusion** (reopening ADR-032): not now.

## Revisit when

A capture shows content missing behind an authored portal, or a level's
interior walls stay unassigned in numbers that matter.
