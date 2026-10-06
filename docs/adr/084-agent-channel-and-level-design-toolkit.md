---
status: partial
updated: 2026-10-06
authority: adr
---
# ADR-084: Agent channel and level design toolkit

## Status

Accepted (partial). The agent channel, brushes, brush editing, level
checks, entity IO, terrain and population (phases 0 to 5 of the
[level design toolkit](../proposals/level-design-toolkit.md)) are
implemented, with terrain tile levels in
[ADR-085](085-gpu-geometry-lod-and-terrain-geomorphing.md). The toolkit
proposal keeps the tools no phase covered; native Vulkan execution is
unverified.

## Context

Level designers want LLM agents to work in the editor beside them. The Cmd bar
(ADR-075) gives agents text control, but a line holds 255 bytes, one statement
runs per frame, results are text lines, expressions cannot create entities,
and nothing returns an image. An agent that builds part of a level needs typed
operations, batches that undo as one step, structured results, a picture of
the result and a way for the designer to review its work.

## Decision

### Socket and messages

The editor listens on a per-user local socket and speaks newline-delimited
JSON ([editor_agent.c](../../editor/src/editor_agent.c)). Every host uses an
AF_UNIX stream socket
([vkr_local_socket.h](../../lib/src/platform/vkr_local_socket.h); Windows 10
1803 or later through `afunix.h`). Paths are UTF-8, also on Windows, and must
fit `sun_path` (104 bytes on macOS, 108 on Linux and Windows). The editor uses
a private directory `<temp>/vkr`. On macOS and Linux it is `$TMPDIR/vkr`
(`/tmp/vkr` without `TMPDIR`) with mode 0700, and the editor refuses a
directory another user owns or others can open. On Windows it is `vkr` in the
user's temporary directory, owned by the user with a protected DACL that
grants only that user; the editor refuses one another account owns and
restores the DACL on one the user owns. It binds `editor-<uid>.sock` there
(on Windows the uid is the relative identifier of the account SID), under a
0177 umask on POSIX. When a live editor already answers there, the next
editor binds `editor-<uid>-<pid>.sock` and logs the path. A socket file
nobody answers is stale and is replaced. `--agent-socket <path>` and
`VKR_EDITOR_AGENT_SOCKET` choose the path; `--no-agent-socket` turns the
listener off. `vkr_mcp` computes the same default path.

The editor polls the nonblocking listener once per UI build, serves at most 8
clients, closes a client whose unfinished line passes 1 MiB, and queues at
most 64 requests. A ninth client receives one `VKR-AGENT-0008` line before
the editor closes it. A request is
`{"v":1,"id":<number|string>,"op":"<name>","agent":"<name>","args":{...}}`
with `agent` optional; a response is
`{"v":1,"id":<same>,"ok":true,"result":{...}}` or
`{"v":1,"id":<same>,"ok":false,"error":{"code":"VKR-AGENT-NNNN","message":"..."}}`.
The request's author is `args.agent`, else `agent`, else `client<slot>`; the
editor's own requests (Cmd `op`, the Changes window) have none.

Requests from every client run one at a time in arrival order
([editor_agent.c](../../editor/src/editor_agent.c)). A build runs the active
request. While every request the build ran was a quick read (a cheap
operation that changes nothing, such as `editor.status`, `scene.describe` or
`query.raycast`) and 1 ms of the build remains, the next quick read runs in
the same build, so the reads of several agents do not wait a frame each. Any
other request waits for the next build. A request whose edits apply after
the UI build, or whose capture renders later, answers in a later build. A
headless editor (ADR-075) stays open while a client is connected or a request
waits or runs, so a script only needs to outlast the client's connection.

| Code | Meaning |
|---|---|
| `VKR-AGENT-0001` | Malformed JSON or message shape |
| `VKR-AGENT-0002` | Unknown operation |
| `VKR-AGENT-0003` | Invalid or missing argument |
| `VKR-AGENT-0004` | Entity or component not found, or an ambiguous name |
| `VKR-AGENT-0005` | The scene rejected an edit; the batch rolled back |
| `VKR-AGENT-0006` | No scene is loaded, the simulation runs, or another batch is in flight |
| `VKR-AGENT-0007` | Capture failed |
| `VKR-AGENT-0008` | A limit was exceeded |
| `VKR-AGENT-0009` | An agent's undo or redo would take another author's step |
| `VKR-AGENT-0010` | A write or claim touches another agent's claim |

### Operations

One table in [editor_ops.c](../../editor/src/editor_ops.c) defines each
operation's name, description, JSON Schema and handler; `ops.list` returns it,
and the MCP adapter builds its tools from that list. Operations run on the UI
thread during the editor's build.

| Operation | Purpose |
|---|---|
| `ops.list`, `editor.status` | The table, with whether each operation settles; loaded containers with their journal `revision`, selection, simulation, view, pending changes and what still rebuilds |
| `scene.describe`, `entity.get`, `query.bounds` | Entities with ID, name, parent, component types, local pose and world bounds; one entity's components as descriptor JSON |
| `entity.create`, `entity.set`, `entity.delete`, `entity.parent` | Structure and pose; delete with `recursive` deletes descendants first |
| `component.add`, `component.set`, `component.remove` | Component values by descriptor property name, partial for `set` |
| `batch` | Several write operations as one journal group |
| `changes.list`, `changes.accept`, `changes.reject` | Review of agent edits |
| `changes.feed`, `claims.set`, `claims.release`, `claims.list` | What every author changed since a sequence number; boxes that keep other agents out |
| `undo`, `redo`, `cmd` | Cmd statements through the Cmd queue, returning the `[cmd]` lines they printed |
| `query.raycast` | First physics surface along a ray |
| `view.capture` | A PNG of the Scene or the whole window, optionally from another view, framed on an entity or box, with grid labels |

An entity argument is `"<world>:<index>:<generation>"`, a unique exact name,
or `"$k"` for the entity operation `k` of the same batch created. Component
values read and write through `vkr_type_read_json_document` and
`vkr_type_write_json`, so they use the same names, units and validation as
scene documents; `set` keeps every property it does not name. Rotations are
degrees XYZ. Lights are set through `component.set` and created through
`entity.create`; physics bodies are added through `cmd`.

### Batches and journal groups

Every write runs as a batch: one `VkrSampleEditBatchRequest` of at most 2,048
`VkrSampleEditBatchItem` edits, all in one container, that the runtime applies
after the UI build ([vkr_sample_runtime.c](../../runtime/src/vkr_sample_runtime.c)).
The editor validates every argument and component value before submission.
The runtime opens a journal group, applies the edits in order and resolves
`$k` references to the entities earlier creations returned. The first failed
edit undoes the group's applied entries, drops them and reports the failing
index with the journal status. `dry_run` validates without submitting. Edits
wait while the simulation runs, as Details edits do.

A journal group ([vkr_scene_edit.c](../../runtime/src/renderer/systems/vkr_scene_edit.c))
tags each entry appended between `vkr_scene_edit_group_begin` and
`vkr_scene_edit_group_end`. Undo and redo move over a whole group; a failure
inside one stops there, as between two entries. The journal holds 4,096
entries; eviction removes the oldest whole group, and a group past 2,048
entries, one full batch, fails. `vkr_scene_edit_group_rollback` undoes and drops the open group.

### Settling

An edit rebuilds brushes and shapes, terrain meshes and collision, and
scatter and spline copies in later updates: a moved brush waits for its
transform to rest, terrain collision waits eight updates after the last
edit, and a scatter waits for terrain collision. A check that ran at once
would read the old scene. A read of collision or built geometry
(`query.raycast`, `query.reachable`, `query.bounds`, `level.lint`,
`level.map`, `view.capture`) therefore first waits until no brush or shape
rebuilds or waits for its uploads (`vkr_scene_brush_pending`), every terrain
is settled, no population rule waits (`vkr_scene_population_pending`) and no
scene loads, unless `settle` is false. After 240 builds it reads anyway and
answers with `settled` false. `scene.describe` and `entity.get` wait only
with `settle`. A capture waits again after it changes the view, as the new
view can stream terrain tiles in.

A write with `settle` answers once the scene settles and adds each
operation entity's world bounds and build status to its result, so one
request applies, rebuilds and measures. `editor.status` reports what still
rebuilds under `rebuilding`. The scene does not rebuild while the simulation
runs, so Play counts as settled. Texture and mesh streaming do not count.

### Review

A batch with `review` (the default) becomes a pending change: its group,
container, label (`label`, else the operation's name) and the entities it
created or edited, held in memory by the operation table. The Agent changes
window (View menu, Cmd `window changes`) lists pending changes newest first,
one card each: the author as a coloured pill, the label, the objects, the
container and the age, with Focus, Reject and Accept. With changes from more
than one author, chips filter the list by author; a filtered list offers
Reject for all of its changes, newest first, after a confirming second click
within 3 s, because rejected work leaves no redo. A refused reject keeps its
card, outlined in red, with the reason: the later pending change that blocks
it and must be rejected first, or the entity a later edit changed. The Scene
outlines pending entities' local bounds in orange through the editor's line
overlay, and a toast announces each new change. Claims (below) list after
the changes. Accept removes the mark
only. Reject calls `vkr_scene_edit_group_revert`:

- an undone group only loses its redo entries;
- otherwise no later applied entry may name an entity the group created,
  deleted, edited or reparented, a current descendant of one, or a parent of
  one as its own parent; a later physics batch or collision-layer edit also
  refuses. The group's entries then revert in reverse order and leave the
  journal, and the redo entries above the cursor are dropped.

A refused reject names the conflicting entity. A change disappears when its
group leaves the journal, as after a scene reload or an undo followed by a new
edit.

A change records its author, which `changes.list` returns and the Changes
window shows before the label. Undo follows the newest entry across every
journal, so an agent's `undo` would take whatever another agent or the
designer did last. The operation table therefore remembers the groups of the
newest 64 agent batches, reviewed or not, with their authors. An agent's
`undo` or `redo` runs only when the step it would take belongs to one of its
own batches, and otherwise fails with `VKR-AGENT-0009` naming the step's
author; the agent rejects its change instead. The editor's own requests and
the Cmd `undo` stay unrestricted.

### Working beside other agents

An agent claims the box it builds in with `claims.set` (a region, a name
and a container; `claim` moves one of its own). A claim that overlaps
another agent's is refused; boxes that only touch do not overlap, so
neighbouring regions share a wall plane. Another agent's write that touches
a claim fails with `VKR-AGENT-0010`, naming the claim and its author:

- before it applies, by the current box of each existing entity it edits
  or deletes, and by the world rectangle each terrain edit may change
  (`vkr_heightfield_op_rect`, mapped as the journal maps it);
- after it applies, by the new box of each object it created or moved; the
  editor then reverts the batch, the newest group, in the next build's edit
  slot before it answers.

An object's box is its solids' (brushes and blockout pieces, from their
faces, `vkr_editor_entity_world_box`), else its meshes', else its position,
so a mesh whose model still loads counts as a point; terrains count only by
their edits. The designer's edits ignore claims. At most 64 claims live in
memory with the operation table and leave with a scene reload;
`claims.release` frees one or all of the caller's, the editor's own
requests may free any, and the Changes window lists claims after the
changes, filtered with them, with Focus and Release claim. The Scene draws
each claim as a blue box through the line overlay.

`changes.feed` returns, oldest first, what happened after a sequence number:
each batch applied by any author, the editor's own included, each change
accepted or rejected, and each claim set or released, with its author,
label, objects (16 at most) and the box around them. The operation table
keeps the newest 128 events; `next` continues the reading, `missed` says
older events left the ring, and each container's journal `revision` grows
with every edit, the designer's too. An agent polls the feed instead of
paging `scene.describe` to learn what the others did.

### Captures

`view.capture` optionally switches the camera view and grid labels and frames
an entity's world bounds or a box (`VkrSampleViewRequest.frame_box`). After
four builds, and once the scene settles, it asks the runtime for one
`final_color` capture
(`VkrSampleCaptureRequest`). The runtime marks editor captures with the high
bit of the request id, lends the poll result to the next build and releases it
after that build; the harness owns the capture slot when it runs, and the
request then fails. The editor converts RGBA8, BGRA8 or half-float color to an
RGBA8 PNG of the Scene image rectangle, or of the whole window with `area`
`window`, writes it to `$TMPDIR/vkr/captures/` (on Windows `vkr\captures`
in the user's temporary directory), keeps the newest 32, and
restores the previous view, grid labels and camera.

### MCP adapter

[vkr_mcp](../../tools/agent/vkr_mcp.c) is an MCP server over stdio that the
editor build produces and the editor distribution installs. It implements only
the [2026-07-28 revision](https://modelcontextprotocol.io/specification/2026-07-28/changelog):

- every request must carry `io.modelcontextprotocol/protocolVersion`
  `2026-07-28` in `_meta`; another version, a missing one or an `initialize`
  request returns `UnsupportedProtocolVersion` (-32022) with the supported
  versions;
- `server/discover` returns the version, the `tools` capability, usage
  instructions (the verification order: plan as data and send one batch,
  measure with queries and `level.map`, then capture) and the server
  identity;
- `tools/list` maps each operation to `vkr_<operation with dots as
  underscores>` in table order with `ttlMs` 60000 and `cacheScope` `private`;
- `tools/call` forwards the arguments to the editor and returns the result as
  `structuredContent` and as JSON text, an editor error with `isError`, and a
  capture's PNG also as image content;
- every result carries `resultType` `complete` and `serverInfo` in `_meta`;
  notifications are ignored and logs go to stderr.

The adapter reconnects once per call, so it survives an editor restart.
`--agent <name>` (or `VKR_AGENT_NAME`) adds `agent` to every request, so
each agent of a swarm that runs its own adapter is its own author; a call's
own `agent` argument takes precedence. It links Bakery's JSON tree, which the
`vkr_bakery_json` library now shares with Bakery and the editor.

### Brushes

A brush is an entity with a `brush` component (`role`: solid, visual, clip
or trigger) whose direct children carry `brush_face`: an outward `normal` and
`distance` in the brush's space, a `material` file, and texture `uv_offset`,
`uv_scale` (meters per repeat), `uv_rotation` and `uv_world`
([vkr_scene_types.c](../../runtime/src/renderer/systems/vkr_scene_types.c)).
Faces are entities because a component value holds at most 1,024 bytes and
descriptors have no arrays. The journal refuses transform edits of a face,
deleting a brush deletes its faces in one journal group, the Outliner and
Content hide faces, and object icons skip brushes.

[vkr_brush.c](../../runtime/src/level/vkr_brush.c) builds a brush on the CPU:
each face plane's square is clipped by every other plane in double
precision, vertices weld within 1e-4 m, and a brush with fewer than 4 or more
than 64 faces, a zero or non-finite plane, a face that does not touch the
solid, an open solid or one without volume fails with the reason and the
face. UVs project on the world axis plane nearest the face, as Hammer does:
floors use X and Z, walls read left to right from outside with up the world.
It also generates box, wedge and cylinder planes, and the lightmap chart
layout scene bakes use for brushes ([ADR-088](088-baked-lightmap-sets.md));
scene bakes build solid and visual brushes from their faces
([ADR-054](054-baked-diffuse-volumes.md)).

[vkr_scene_brush.c](../../runtime/src/renderer/systems/vkr_scene_brush.c)
rebuilds a brush after a change to it or its faces, or after its transform
moved and then rested for two updates, at most 256 brushes per update. A
rebuild makes one generated mesh with one submesh per face material (16 at
most), attached as the entity's runtime shape through
`vkr_scene_attach_generated_mesh`, so transform sync, picking, visibility and
the Show filter follow the shape path. Materials load once per scene and
path; an empty path uses `assets/materials/dev/dev_grid.mt`, one of seven dev
grid materials shipped as engine content. Clip and trigger brushes draw only
while `VkrScene.editor_volumes` is set, which the editor runtime sets while
it edits and clears during Play; games leave it off.

Solid and clip brushes add their world-space convex hull to their 32 m world
cell; each cell holds static bodies of at most 32 hulls each, so a raycast's
collider names the brush. A trigger brush owns a sensor hull. These are
generated bodies (`vkr_scene_physics_generated_set`): the scene creates and
destroys them, and snapshots, documents, the journal and Reset never see
them. The first generated body creates a scene's physics state when the
scene had none.

Operations `brush.box`, `brush.wedge`, `brush.cylinder`, `brush.stairs`,
`brush.set_material` (faces by `top`, `bottom`, `sides`, `+x`, `-x`, `+z`,
`-z`), `blockout.room` (floor, ceiling and four walls around an interior box),
`blockout.corridor` and `blockout.doorway` (an unscaled axis-aligned box wall
becomes up to three brushes around an opening) build ordinary brushes.
Stairs and corridors are editable blockout shapes: an entity with a
`blockout` component (`SceneBlockout` in
[vkr_blockout.h](../../runtime/src/level/vkr_blockout.h)) that the scene
builds the way it builds a brush. The layout in
[vkr_blockout.c](../../runtime/src/level/vkr_blockout.c) turns the settings
into convex pieces, each the hull of its points. One generated mesh holds
them, in chunks of 512 pieces with a submesh per chunk and material (floors
take the floor material), textured in world space and without a lightmap.
The shape owns one static body whose triangle mesh holds every piece's faces.
A shape has no child entities, so a change of its settings is one undo
entry at any size, and a document stores only the component. The component
holds the shape and the following settings:

- the stairs kind;
- height, width, length and step height;
- turn and radius;
- the thickness of corridor walls or of floating steps;
- the turn direction and the ceiling;
- up to 16 corridor points in the group's space, each with its own corner
  radius or the shape's;
- up to 8 wall openings, each a wall (stretch and side), a span along it
  and a span above its floor;
- the materials.

`blockout.create` adds one, `blockout.build` (or `component.set` of
`blockout`) sets its settings, refused when they would not lay out, and
`blockout.bake` turns it into plain brushes under its entity in one undo
step. Bake refuses a shape of more pieces than one batch of 2,048 items
holds, each brush taking one item and one per face. The kinds of stairs
are:

- `straight`;
- `l`: two flights joined by a square landing, the second turned 90 degrees
  left or right; the length includes the landing;
- `u`: two flights joined by a landing, the second running back beside the
  first;
- `curved`: steps along an arc around a center beside the stairs, its inner
  radius `radius`, sweeping `turn` degrees;
- `spiral`: steps around a pole of `radius`, turning up to 92,160 degrees.
  Past one turn its steps float, each at least a rise thick, so no step
  buries the flight below it. `brush.stairs` turns a spiral 22.5 degrees a
  step, at least once, unless `sweep` says otherwise.

Steps share the height evenly, each at most `step_height` high, 4,096 at
most, on stairs at most 1,024 m high. They are solid down to the floor, or
slabs `thickness` thick. A corridor runs along its points with a floor, two
walls mitred at the bends and an optional ceiling per stretch. Each corner's
radius rounds it with an arc tangent to both stretches, shrunk to fit them,
in pieces of about 10 degrees, at most 12 per corner and 256 stretches in
all. A wall with openings splits into the pieces beside, above and below
each. `vkr_blockout_piece_capacity` gives the pieces a shape may lay out,
which the scene, the operations and the previews allocate; previews of
longer shapes outline every n-th piece, at most 112. The journal keeps 2,048
changes per group and 4,096 in all.
`brush.stairs` and `blockout.corridor` create these shapes from the earlier
arguments. `entity.place` moves an existing object by world boxes,
as one `entity.set` of its position: `on` sets its bottom on the target's
top, `inside` on the target's floor (`vkr_editor_entity_floor`, a room's
floor slab), both centred unless `keep`; `against` sets it flush to the
target's `side` (+x, -x, +z or -z), centred along that side unless `keep`;
`gap` leaves metres between them. `brush.snap` moves 2 to 16 objects against the
first. Each other object, unless it carries `free_placement`, moves the least
distance that sets its world box flush against a side of an object placed
before it, `gap` apart, lined up with that object's sides or center on the
other two axes. The box is the one around an object's brushes and shape
pieces and those below it, else around its meshes and shapes
(`vkr_editor_entity_world_box`).
`entity.parent` with `snap` first sets the child flush against its new
parent the same way, unless it is free; the reparent then keeps that pose.
A child over the parent's footprint rests on top unless it lies wholly
below the parent's middle, where it hangs beneath. Beside it, the child moves
up or down so the two floors meet: an object's floor is the top of the slabs
at its bottom when they cover a quarter of its footprint, as a room's or a
corridor's floor, else its bottom, as for stairs. A slab is at most 0.5 m or
a quarter of its shorter side thick (`vkr_editor_box_slab`), and a parent
brush's floor is its top when it is one.
Every operation snaps
corners to `grid` (1/16 m unless set; 0 turns snapping off) and validates the
solid before submission. `scene.describe` and `entity.get` summarise a
brush's role, face count, build status and materials and list faces only with
`faces`. `view.camera` places the perspective camera, and `view.capture`
accepts `eye` and `target`.

In the editor, the Create menu's Level group adds Brush Box, Brush Wedge,
Brush Cylinder and Blockout Room at the placement point. Brush drawing (B,
View > Draw brushes, Cmd `brush.draw`) takes the Scene's mouse and draws in
two steps, as Chisel and Source 2 Hammer do: a left drag outlines the base,
then the pointer raises the box from one step and a click creates and
selects it; Escape cancels the box, then ends drawing. Between boxes the
selected brush keeps its face handles, and a press on one drags that face
instead of starting a box. The Scene's Snapping menu picks where the
box starts and its steps. Surface starts it on the upward-facing collision
surface under the pointer, so a brush drawn on another brush sits on top of
it, and falls back to the grid plane. Grid draws on the grid plane. Both keep
the corners on grid crossings and the height in whole cells; Free draws on the
grid plane in 1/16 m steps. A new box, wedge, cylinder or stairs takes the
palette's role and, when solid or visual, its last Material swatch.

The stairs tool places stairs in the palette's kind and turn with a drag
from where they start toward where they go, on a surface with Surface
snapping. Straight, L and U stairs run as far as the drag, and a spiral
reaches it with its rim. A click places them facing away from the camera.
The corridor tool adds a floor point per click. A click on the last point
again, or Enter, builds the corridor, its corners rounded by 2 m arcs when
the palette says Curved. Backspace drops the last point. Both draw the
shape they would build, and Escape steps back before it ends the tool.

With the Select tool a selected shape shows handles
([editor_blockout.c](../../editor/src/editor_blockout.c)):

- stairs drag their length, height and width along arrows, and curved and
  spiral stairs their turn, in 15 degree steps, and their radius;
- dragging a spiral's top or turn keeps its pitch, so the stairs climb on
  as they turn;
- corridors drag their points on their level, a + between two points adds
  one there, Ctrl+click on a point removes it, a handle in each bend sets
  that corner's radius, and arrows set the height and width.

A selected corridor shows the face grid of the wall under the pointer, where
only cells answer: Delete with cells selected cuts them as an opening of the
shape, which stays editable. On a brush, Delete pushes the patch through the
brush.

Lengths follow the grid step, or 1/16 m with Free snapping. A drag previews
the shape and its release builds it. Details shows the component's rows for
the shape's kind. A row's drag previews the same way and builds once it
ends. Bake in the palette runs `blockout.bake`.
Doorway in the palette cuts the patch selected on a
brush's face grid through the brush, through `brush.patch` pushed past the
brush's depth behind the face. A door, window or arch opening then has the
patch's exact size, which the hint shows. Without a patch it runs
`blockout.doorway`. Collision display draws the convex hulls of solid and
clip brushes and of shape pieces, the selected one's or every one's (at most
256 objects), as it draws physics colliders. Cmd
`op <operation> [json]` runs any operation of the table.

With the Snapping menu's magnet on (default; Cmd `view.snap_magnet`), objects
snap to nearby brushes within 2 % of their distance from the camera (0.05 m
to 2 m), about 15 to 20 pixels on screen. A move by the gizmo or a
Select-tool drag shifts the object, per axis, onto the nearest of: flush
against a neighbour it overlaps on the other two axes, or level with a
neighbour's side, top or bottom when the two touch. The object can be a
brush, a group of brushes such as a room, a shape or a mesh, by its world
box; its own brushes and pieces are no neighbours. An object with an enabled
`free_placement` component never snaps: Free in the palette adds it to the
selection or clears it. The runtime keeps the snapped
position on an axis handle's axis or a plane handle's plane
(`VkrSampleUiClient.snap_move`). A drawn box's corners snap onto the sides of
brushes beside them and its top onto their tops and bottoms. The magnet
compares axis-aligned world boxes of the other brushes, triggers excepted,
and of shape pieces, gathered once when a move or a box starts
([editor_level.c](../../editor/src/editor_level.c)), so a slanted face snaps
by its box.

Dragging an Outliner row onto another row of the same scene parents the
object under it (`entity.parent` with `snap`) as one undo step. A reparent
keeps the world pose computed from the local values along both chains, so a
parent created or moved earlier in the same batch places the child
correctly. Dropping it
on the list's empty space makes it a root again.

### Brush editing

Editing operations replace brushes in one journal group, keeping each
derived face's material and texture settings from the face it copies
([vkr_brush.c](../../runtime/src/level/vkr_brush.c), `VkrBrushPiece`). An
operation that replaces a brush keeps it as its first piece: the faces that
piece keeps take their new planes, its new planes become new faces and the
faces it drops go, so the brush keeps its id, its components, scripts and
connections and the references to it. Further pieces are new brushes:

| Operation | Result |
|---|---|
| `brush.move_face` | Moves one face (`face`, or `brush` and `side`) along its normal by `distance`; refused when the solid would break |
| `brush.extrude` | A new brush grown out of a face by `distance` |
| `brush.clip` | Cuts a brush with the world plane through `point` facing `normal`, keeping `back`, `front` or `both` pieces |
| `brush.hollow` | Replaces a brush with walls of `thickness` around its inside |
| `brush.carve` | Subtracts a `cutter` from `target`, or from every brush it touches, as non-overlapping convex pieces, at most one per cutter face; deletes the cutter unless `keep_cutter` |
| `brush.merge` | Joins 2 to 8 brushes into one when their union is convex: the merged solid's volume must equal the sum of theirs |
| `brush.patch` | Pulls a rectangle of a face's grid (`min` and `max` as `[u, v]` on the face's grid axes) out by `distance`, or pushes it in when negative. Pulled, it joins the brush when the union stays convex (a whole face stretches the brush) and is a new brush otherwise; pushed, it carves a recess or a hole. Every new face copies the face's material |
| `brush.reshape` | Moves brush corners at `points` by the world `delta`: a corner, an edge's two ends, or with `split` {`point`, `normal`} a grid line's ends after cutting the brush along that plane. A piece whose moved corners stay on its hull becomes that hull. A piece a moved corner dents becomes the solid bounded by its faces through the moved corners (a face no longer flat bends along the line between the unmoved corners beside the moved ones), cut along its face planes until every part is convex: at most eight pieces in one batch, the brush keeping the first. Refused when the dent needs more pieces or a face folds through another |

Carving keeps no boolean tree: pieces are ordinary brushes. Operations work
on unscaled brushes, since a scaled brush's planes are not the planes the
designer sees.

A brush selected with the Select tool (Q) shows its edges, its corners and,
on each face turned toward the view, a grid on the editing grid's lines; a
face wider than 24 cells doubles its grid step until it fits
([editor_brush_grid.c](../../editor/src/editor_brush_grid.c)). The move
tools keep their gizmo instead. Under the pointer, nearest kind first, the
patch arrow, a corner, an edge, a grid line or a cell lights up and takes the
press from object picking. Targets are measured on screen through the view
projection: corners and the patch arrow within 8 points, edges within 6 and
grid lines within 5, the same in every view:

- A drag across cells selects a patch, a rectangle of cells on one face; a
  second press on the face within 0.4 s selects all of it, and Escape clears
  it. The patch shows an arrow out of its center: dragging it out pulls the
  patch (`brush.patch`), so a strip of a floor becomes a wall; dragging it in
  pushes a recess or a hole through.
- A drag on a corner or an edge moves it along the normal of the face it was
  picked on, or along that face with Shift (`brush.reshape`). Ctrl+click
  gathers up to eight corners from corners and edges, or takes them out
  again; a drag on a gathered one moves them all, so two raised top edges
  make a gable. With Ctrl held only corners and edges answer, and
  Ctrl+click elsewhere still adds objects to the selection.
- A drag on a grid line splits the brush along it and moves the line the
  same way, so a ridge or a valley bends the face into two brushes.

Moves snap to the grid step, or 1/16 m with Free snapping. A drag previews
its result in orange, a dent as its convex pieces, and refuses in red a dent
that needs more than eight pieces or folds the brush, with the reason in the
hint under the Scene. While boxes are drawn, cells start boxes and only corners,
edges and grid lines take presses. The other limits have next steps in
the
[Level design toolkit proposal](../proposals/level-design-toolkit.md#face-grid-follow-ups).
Alt+click selects the brush face under
the pointer instead of its object, which then shows its outline and a handle
out of its center that pushes or pulls it, and Alt+Up or Alt+Down moves it
0.25 m out or in (1 m with Shift).

The clip tool (Shift+X, View > Clip brushes, Cmd `brush.clip_tool`) cuts on
the same grid, shown on the selected brush with any transform tool; without
a selection a click selects a brush. A click on a grid line cuts the brush
along it, through the plane perpendicular to that face. Corners, the points
where edges cross the face grid and grid crossings light up as points: a
click sets the first point and a second click cuts straight across the first
point's face through both; Shift+click on the second point holds it, and a
third click cuts through all three for a slanted cut. Once the cut has a
point, the pointer anywhere on a face takes its nearest crossing. The cut
the pointer would make previews its two pieces in orange with the new faces
lit, and a plane that misses the brush is refused in the hint. `brush.clip`
keeps both pieces and selects the first. Escape drops the points, then ends
the tool. All of these submit the operations above through the agent queue,
so they undo like agent work.

### Level checks

[editor_level.c](../../editor/src/editor_level.c) samples a region on a grid
of capsule-radius cells (at most 65,536) with physics raycasts. Each cell
keeps every floor a downward ray finds from start heights one capsule height
apart, so floors under roofs count. A floor is walkable when its slope is
within `max_slope_radians`, a capsule's height fits above it and side rays
find no wall closer than the radius. Neighbouring floors connect when the step
up is within `step_up`, a drop is at most 4 m, and nothing blocks the way at
knee height. Only collision counts; geometry without collision is invisible
to the checks. The capsule defaults to `vkr_physics_character_default`
(radius 0.3 m, height 1.8 m, `step_up` 0.35 m, 45°) and each request may
override it.

`level.lint` reports steps too high, slopes too steep, ceilings lower than the
capsule, gaps narrower than its diameter, walkable edges with no floor within
4 m below, walkable areas the `start` (or the first enabled Player Start)
cannot reach, overlapping solid brushes and brushes that did not build. Each
issue names its position, the entity at fault and the step height, slope,
headroom or gap; issues of one kind on one entity within 8 m merge.
`query.reachable` flood-fills from `from` and returns whether `to` is
reachable with one route and its length. The grid's cell grows with the
region past 65,536 cells of the capsule radius: a square up to 76.8 m keeps
0.3 m cells, while a 256 m square samples every metre and can miss a gap
narrower than the capsule.

`level.map` returns the same grid as text, a floor plan a model reads more
exactly than a picture ([editor_level.c](../../editor/src/editor_level.c),
`vkr_editor_level_map`). Its rows run from the region's minimum z to its
maximum, each character a cell along +x, as a top capture shows them. A cell
shows its highest walkable floor, else its highest floor: `.` walkable and,
with a start, reached from it, `,` walkable but out of reach, `S` the start,
`#` too close to a wall, `n` a gap narrower than the capsule, `_` a ceiling
too low, `/` too steep and `-` no floor. `cell` sets the cell edge (by
default 96 cells along the longer side) and a map holds at most 200 a side;
`heights` adds each shown floor's world height. A region whose top lies
below the ceilings maps the floor under them.

The Level checks window (View > Level checks, Cmd `window level`) runs the
lint over 40 m around the point the Scene's center looks at, lists issues
nearest first with Focus, and marks them with crosses in the Scene while it
is open.

### Entity IO

Entities talk through outputs and inputs, as Source's IO does. A type
declares the outputs it fires and the inputs it handles on its descriptor
(`VkrTypeDesc.outputs` and `.inputs`); the engine components in
[vkr_scene_types.c](../../runtime/src/renderer/systems/vkr_scene_types.c)
are:

| Component | Outputs | Inputs |
|---|---|---|
| `trigger` (`enabled`, `once`, `filter` component) | `on_enter` and `on_exit` (the other entity), `on_empty` | `enable`, `disable`, `toggle` |
| `relay` | `on_trigger` | `trigger`, `enable`, `disable` |
| `timer` (`interval`, `start_running`, `once`) | `on_timer` | `start`, `stop`, `set_interval` |
| `counter` (`start`, `min`, `max`) | `on_changed` (the value), `on_max`, `on_min` | `add`, `subtract`, `set` |
| `mover` (`direction`, `distance`, `lip`, `speed`, `wait`, `start_open`, `loop`, `locked`) | `on_open`, `on_opened`, `on_close`, `on_closed` | `open`, `close`, `toggle`, `lock`, `unlock`, `set_position` (0 to 1) |
| Every entity | none | `show`, `hide`, `destroy` |

Script behaviors declare theirs with `VKR_OUTPUTS` and `VKR_INPUTS`
([ADR-079](079-c-script-modules.md)); the host copies them onto its type
copies, so connections and agents see them without a session. A
connection is a child entity of its source with one `io_connection`: the
output name, an `ENTITY` reference to a target in the same container
([ADR-076](076-project-object-model.md)), the input name, an optional
value as text that replaces the output's value, a delay in seconds and a
fire limit. Names may carry their component, as `trigger.on_enter`.
Connections and brush faces are parts (`vkr_scene_entity_is_part`): lists
hide them and deleting the owner deletes them in the same journal group.

The script host owns one router per session
([vkr_io_router.c](../../runtime/src/script/vkr_io_router.c)):

1. **Publication.** At session start it resolves every connection to its
   source, target, ports and value, and reads the engine components' state.
   An invalid connection is logged with its reason and never routes; a
   zero-delay loop is a warning. The same check serves agents and the
   editor (`vkr_io_connection_problem`).
2. **After each tick**, after the tick's queued edits, it drains the
   simulated scene's sensor events. It is then the only drain; outside a
   session the sample runtime discards them. Each side of a pair gets its
   behaviors' `trigger_enter` or `trigger_exit` hook, and a `trigger`
   component turns filtered entries and exits into outputs. Due timers fire
   next, then outputs scripts fired during the tick, in order.
3. **Delivery.** Zero-delay deliveries run first in, first out; inputs may
   spawn and destroy at once. A delayed delivery waits for the first tick at
   or after its deadline in simulation time. Outside a tick, as from an
   input handler, an editor request or a script's `update`, firing delivers
   at once.
4. **Bounds.** Queues hold 4,096 deliveries each and are reserved at
   publication. More than 4,096 deliveries in one tick or a chain of more
   than 64 zero-delay hops faults the session with the place it happened;
   nothing is dropped or deferred silently. A delivery to a destroyed
   target is dropped and counted.
5. **Trace.** Each delivery logs
   `[io] 12.350 Lobby trigger.on_enter(Player) -> Door A.open`; `io.trace`
   turns it off.

A `mover` moves its entity and everything under it between its saved pose
and an open pose `distance` meters along `direction` in its own space, as
Source's func_door and func_movelinear do; a zero distance takes the extent
of its meshes and shapes along the direction less `lip`. It moves only
during Play: publication and refresh give each mover an evaluated transform
at its pose, `vkr_io_router_step` rewrites it first in each tick's
`before_physics`, and clearing the router removes it, so the saved transform
never changes and children follow through the transform update. Setting off
or turning around fires `on_open` or `on_close`; reaching an end fires
`on_opened` or `on_closed`. At the open end it closes after `wait` seconds
unless `wait` is negative; `loop` sets off at session start and turns back
after `wait` (at least zero) at each end; `locked` refuses `open`, `toggle`
and `set_position` but still closes. Solid and clip brushes under a mover
(the nearest at or above them) join one kinematic generated body per mover
instead of a cell, at most 32 hulls; the hulls stay in world space at rest
and the body's kinematic target is the mover's world offset, so a character
on it reads its ground velocity. The mover's motion never rebuilds its
brushes, and their own edits wait until its evaluated pose clears; trigger
brushes and blockout shapes under a mover keep their static bodies at rest.
A reset rebuilds kinematic generated bodies at rest; an origin rebase moves
each generated body's origin, and targets stay offsets from rest. Generated
body keys: cells set bit 63; trigger brushes bits 63 and 62; blockout shapes
bit 62 over the entity's index and generation; movers bits 62 and 48 over
the same; terrain bodies are the entity id XOR "terrain".

Physics serves triggers in two ways the toolkit needed. Sensors test
character capsules directly, so the player enters triggers
([ADR-073](073-native-gameplay-foundation.md)). Generated bodies count as
bodies, so a scene whose only collision is brushes still steps. Each
generated body keeps its own copy of its colliders, and a reset rebuilds it
in the replacement world.

In the editor ([editor_io.c](../../editor/src/editor_io.c)), Details shows
an object's connections under Outputs, with `+` adding one from its first
output, and the connections that reach it under Inputs; each row selects the
connection or its source and warns with the reason a connection will not
route. A selected connection shows its component's fields, a Route section
with that reason, its source and target, Pick target (the next object
selected in the Scene or Outliner becomes the target) and the ports both
ends offer. The Scene draws amber lines from the selection to its targets
and blue lines from the sources that reach it. Connections and faces show no
transform, script, component or physics rows. The Create menu's Level group
adds Trigger Volume (a trigger brush with `trigger`), Relay, Timer and
Counter. Cmd `io.trace` and `io.fire <object> <input> [value]` work in the
bar during Play, with script instances or engine IO components alone, and
`level.lint` reports connections that will not route as
`broken_connection`. The Level Design palette's Mover button and operation
`mover.create` (`objects` that share one parent, `name`, and `values` over
the mover's defaults) group the objects under a new entity carrying a
`mover` at the center of their world box, as one batch. The Create menu's
Level group adds Mover. Outside Play, the Scene draws the selected mover's
box at its open pose and a line to it.

Operations `io.connect` (`source`, `output`, `target`, `input`, `value`,
`delay`, `limit`; a target the same batch creates is allowed, because a
created entity's id is chosen when the batch builds), `io.disconnect`,
`io.list` (an entity's ports, its connections and the incoming ones, each
with its problem), `io.fire` (an input during Play) and `io.trace` serve
agents and the Cmd bar's `op`. Any write operation takes `select` to select
what it made.

### Terrain

A `terrain` component names a heightfield file, four layer materials
(`layer0` to `layer3`) and a texture size in metres
([vkr_scene_types.c](../../runtime/src/renderer/systems/vkr_scene_types.c)).
A heightfield ([vkr_heightfield.c](../../runtime/src/level/vkr_heightfield.c))
is a square of `cells` cells per side, a multiple of 64 up to 8,192 (above
1,024, a multiple of 1,024), at a sample spacing. Each sample holds a 16-bit
height quantized between the field's minimum and maximum and four 8-bit
layer weights that sum to 255. The file (`VKRHFLD1`) stores samples in tiles
of 64 by 64 that can be read one by one; version 2 adds an overview of every
16th sample after them. A resident terrain's writes go to a temporary file
that replaces the old one; a streamed terrain writes its changed tiles and
overview in place.

The scene owns its terrains
([vkr_scene_terrain.c](../../runtime/src/renderer/systems/vkr_scene_terrain.c)),
at most eight per scene. When the component appears it loads the file and
keeps every sample in memory; a terrain larger than 1,024 cells streams
instead ([ADR-086](086-world-partition.md#terrain-streaming)). Edits change
those samples, mark the 64-cell tiles they touched and the terrain's
collision dirty, and saving the scene
writes each changed terrain's file. Each update rebuilds the marked tiles:

- **Mesh.** One generated mesh holds a submesh per tile, each with its own
  geometry: a 65 by 65 vertex grid with a skirt two spacings deep around it,
  so that tiles cull one by one and no gap opens between them. Each tile
  carries seven detail levels that the GPU selects and morphs between
  ([ADR-085](085-gpu-geometry-lod-and-terrain-geomorphing.md)). UVs are
  local x and z over the texture size, and each vertex color holds its
  sample's four layer weights.
- **Material.** The terrain owns one terrain material that blends its four
  layers (see below).
- **Collision.** One static body with a Jolt height field shape
  (`VKR_PHYSICS_HEIGHT_FIELD`,
  [ADR-072](072-entity-collision-and-rigid-body-physics.md)) is rebuilt
  eight updates after the last edit, so a stroke does not rebuild it every
  frame.
- **Holes.** A sample whose four weights are zero is a hole
  (`VKR_HEIGHTFIELD_HOLE_WEIGHTS`): it keeps its height but opens every
  triangle it is a corner of, in the mesh and in the collision alike. A cell
  with a hole corner splits along its (x, z) to (x + 1, z + 1) diagonal, as
  Jolt's height field does, drops each triangle a hole touches and the skirt
  below a tile edge whose triangle went, and its tile keeps level 0 only.
  Collision reads hole samples as `VKR_PHYSICS_HEIGHT_HOLE`. Overview tiles
  draw holes as layer 0. Paint leaves holes alone; filling one returns it to
  layer 0 at the height it kept. `vkr_scene_terrain_ground`, scatter
  placement and `terrain.sample` find no ground over a hole; the editor brush
  still meets it, so Fill can close it.

A terrain material
([vkr_material_loader_replace_terrain](../../runtime/src/renderer/resources/loaders/material_loader.h))
is opaque PBR built from four `.mt` files. Layer 0 supplies the material's
factors and every map. Layers 1 to 3 supply their base color, metallic,
roughness, normal scale and occlusion strength, and their base color, normal
and ORM maps into nine texture slots of their own
(`VKR_TEXTURE_SLOT_LAYER1_BASE_COLOR` onward), which stream like any
material's. An unnamed layer 0 is the dev grid and an unnamed later layer
plain white; `terrain.create` names the dev grid, floor, orange and blue
materials by default. Each backend publishes the extra layers in a cold
terrain segment of its material table, beside the transmission segment, and
flags the common row. The visibility-buffer G-buffer resolve
([gpu_draws.metal](../../renderer/src/shaders/metal/msl/world/gpu_draws.metal),
[deferred.slang](../../renderer/src/shaders/vulkan/slang/world/deferred.slang))
reads the interpolated vertex color as weights normalized to sum to one, samples
each layer the weights reach with that layer's base color sampler, and blends
base color, metallic, roughness, occlusion and tangent-space normals in
[terrain_kernel.slangh](../../renderer/src/shaders/shared/terrain_kernel.slangh).
Emission and the other extensions come from layer 0. Forward and
transmission shading never see a terrain material, because publication
rejects one that is not opaque.

The accepted design carried weights in a weight texture. Vertex colors carry
them instead: a vertex is a sample, so the resolution is the same, painting
already rebuilds the touched tiles, and no texture upload path is needed.
Coarser tile levels thin the weights with their vertices.

A terrain sits at its entity's position and ignores rotation and scale.
Its local space is metres from its centre, with heights above the entity.

A terrain edit is one journal entry (`VKR_SCENE_EDIT_ENTRY_TERRAIN`) that
holds the touched rectangle's samples before and after. Edits of one
gesture fold into one entry over the union of their rectangles, so a stroke
undoes as one step. Terrain edits are refused while simulating.

Agents edit regions with operations that take world coordinates:

| Operation | Purpose |
|---|---|
| `terrain.create` | A terrain of `size` metres at `spacing` (default 256 m at 1 m) whose file is `assets/terrain/<uuid>.vkrhf`: a resident one starts in memory and its scene's first save writes the file, a streamed one is written at once |
| `terrain.brush` | Raise, lower, smooth, flatten, paint a layer, cut holes or fill them with a round brush at up to 256 points |
| `terrain.flatten` | Level a footprint at a height, blending over a falloff |
| `terrain.ramp` | A straight slope of a width between two surface points |
| `terrain.stamp` | Add or set heights from a grayscale PNG over a square |
| `terrain.sample` | Ground heights at x and z points, or with `region` a grid every `step` metres in rows from minimum z, each running +x, rounded to centimetres; at most 4,096 heights; null over a hole |
| `terrain.hole` | Cut holes for an entrance, the samples strictly inside a `min`/`max` box (a box on grid lines opens exactly) or within `radius` of `points`; `fill` closes them |

In the editor ([editor_terrain.c](../../editor/src/editor_terrain.c)), the
Terrain window holds the sculpt tool and its mode (Raise, Lower, Smooth,
Flatten, Paint, Hole, Fill), radius, strength and paint layer. Hole and Fill
change every sample inside the radius and take no strength. While the tool is on, the Scene draws the brush circle where
the pointer's ray meets a terrain, and holding the left button applies the
mode every frame as one stroke. Raise and lower move a sample by up to four
times the strength in metres per second. The Create menu's Level group adds
a 256 m Terrain. Cmd `terrain.tool` and `window terrain` reach the same
tool.

### Population

A spline is an entity with a `spline` component (`closed`) whose child
entities with `spline_point` (`order`) are its control points, in the spline
entity's space ([vkr_scene_types.c](../../runtime/src/renderer/systems/vkr_scene_types.c)).
The curve ([vkr_spline.c](../../runtime/src/level/vkr_spline.c)) is a
centripetal Catmull-Rom spline through the points in order, which neither
overshoots nor loops between uneven points; an open curve reflects each end
point's neighbour past it. Samples sit at equal arc length, measured over 32
chords per segment, and an open curve's last sample is its last point.
Points are ordinary children, so the transform tools move them.

Two rules place runtime copies of a cooked mesh (a `.vkb` path and a source
mesh index), each with a rotation in the copy's frame and a scale:

- **`spline_mesh`**, on a spline, places a copy every `spacing` metres along
  it with an `offset` in the copy's frame (x right, y up, z along), upright
  unless it follows the slope.
- **`scatter`** places `count` seeded copies (up to 2048) in its entity's
  box: each drops straight down from the box's top onto the first physics
  surface, upright or leaning to the surface's normal, with a random yaw
  and a scale between `scale_min` and `scale_max`. A column that meets
  nothing places no copy.

The scene owns the copies
([vkr_scene_population.c](../../runtime/src/renderer/systems/vkr_scene_population.c)):
mesh manager instances that documents never store. Each update hashes a
rule's inputs (its component, its entity's transform and visibility, a
spline's points, and for a scatter every terrain's revision) and rebuilds
the rule when the hash changes. The same mesh and count only move; anything
else recreates the copies. A scatter waits while any terrain's collision is
rebuilding. Copies pick as their rule's entity, through the render bridge.
A scene holds at most 64 rules and 4,096 copies; a rule asking for more
gets the remainder and a status saying so. A copy costs 184 bytes of CPU
memory and, per frame, a 144-byte instance row and a 48-byte candidate per
submesh, so the bound stays under 1 MB of each on the 16 GB floor
([ADR-083](083-supported-hardware-matrix.md)).

| Operation | Purpose |
|---|---|
| `spline.create` | A spline through 2 to 256 world points, with an optional `spline_mesh` |
| `spline.sample` | Positions, tangents and distances along a spline, and its length |
| `scatter.create` | A scatter entity with its component values |
| `terrain.road` | Shape a road of a width along a spline, following its heights, as one terrain edit (`VKR_HEIGHTFIELD_OP_ROAD`, a ramp per sampled segment) |

`scene.describe` reports a rule's copies and status. In the Scene, splines
draw as curves, the selected one brighter, and a selected scatter shows its
box; the Create menu's Level group adds a Spline and a Scatter.

## Consequences

Agents and scripts reach every editor feature through typed operations, with
the Cmd vocabulary still available through `cmd`. One journal group per
request makes agent work undoable and reviewable as the unit the agent chose.
Reverting a group out of order is refused instead of guessed whenever a later
edit could depend on it. A capture needs a rendered frame, so it reports the
Scene as rendered, not what the editor shows while rendering is stopped.

Checks wait for rebuilds, so an agent never verifies a stale scene; a scene
that keeps rebuilding, such as terrain streaming around a moving camera,
costs a read up to 240 builds and answers with `settled` false. Quick reads
share a build only after other quick reads, so every other request still
sees all that earlier requests asked for; across clients, the order of
arrival is the only order. A designer working beside agents keeps an
unrestricted undo, while each agent undoes only its own batches.

Claims judge objects by boxes, so a slanted or hollow object near a claim
can be refused although no face enters it, and a write that only moves
something into a claim costs a revert of the whole batch. Claims and the
feed live in memory: an editor restart loses them, and a crashed agent's
claims stay until the designer releases them.

## Alternatives considered

- Extending only the Cmd bar keeps text results, one statement per frame and
  no images.
- A chat panel inside the editor would tie it to one model provider and its
  keys; the socket lets any MCP client connect.
- Applying agent edits to a preview scene would need a second copy of every
  container; real edits with a review mark reuse the journal and the
  renderer.
- Supporting the 2025-11-25 handshake as well was declined by the owner.

An MCP client that sends only 2025-11-25 or older requests cannot connect;
it gets an explicit `UnsupportedProtocolVersion`. The socket admits only the
user's own processes, which can already edit the project's files.

Population copies have no collision and no saved state: a game that needs
either authors entities instead. A scatter does not see brushes moved after
it placed its copies until its own inputs or a terrain change; changing its
seed places it anew. Linked prefabs and meshes bent along a spline remain in
the toolkit proposal.

A resident terrain that `terrain.create` makes is staged
(`vkr_scene_terrain_stage`): its field starts flat in memory and the first
save of its scene writes the file, so discarding the scene or undoing the
creation leaves no file. A streamed terrain, larger than 1,024 cells a side,
writes its file at once and leaves it behind when the scene is discarded. A resident terrain keeps
all its samples: the largest, 1,025 samples a side, holds 6 MiB of samples
and 256 tile geometries. The owner chose all-resident terrain up to 1 km at
1 m spacing; larger terrains stream by tiles
([ADR-086](086-world-partition.md)). Imported, cooked terrain
meshes were declined in favour of heightfields built in the scene.

Brush meshes are not merged: a level pays one draw per brush. A
measurement put that at about 0.4 µs per brush (see Evidence). Brush
collision follows the brush only after its transform rests, so a dragged
brush collides at its old place until the drag ends. A rebuilt brush keeps
drawing its previous mesh until the new geometry and materials settle, as a
terrain does (level toolkit audit A1); a brush without a mesh, such as a new
one, takes its mesh at once and shows it as the uploads finish.

## Revisit when

An agent needs notifications pushed to it,
reviews must survive a scene reload, or a level's brush count makes the
per-brush draw cost visible next to its other geometry (merge per cell and
material then).

## Evidence

- `./build_test.sh` suite `scene_edit` covers grouped undo and redo, rollback,
  out-of-order revert, its refusal for a dependent later entry, the group
  limit and whole-group eviction (2026-10-04, macOS Debug).
- Headless macOS Release runs on Bistro through the socket and through
  `vkr_mcp` (2026-10-04): status, describe, create, a batch with `$k`
  references, validation and rollback failures, out-of-order reject, undo and
  redo of a batch, `cmd`, a top orthographic and a perspective capture, the
  Agent changes window with Focus, Reject and Accept and the orange outlines
  in a whole-window capture, the -32022 answers to `initialize` and to an older
  version, and 21 tools from `tools/list`.
- `./build_test.sh` suite `brush` covers box, wedge and cylinder volumes,
  winding and plane residuals, the open, empty-face, non-finite, flat and
  face-count rejections and UV projection; suite `scene_edit` covers a
  brush's delete with its faces and its undo (2026-10-04, macOS Debug).
- Headless macOS Release on Bistro (2026-10-04): `blockout.room`,
  `blockout.doorway`, `brush.stairs`, `brush.wedge`, `brush.cylinder` and a
  trigger `brush.box`, collision raycasts hitting the room's ceiling and
  passing through the doorway, rejected flat and ambiguous requests, a brush
  drawn with `ui.drag`, and top and perspective captures.
- `./build_test.sh` suite `brush` covers carve (pieces, no overlap, the
  disjoint and swallowed cases), prune, extrude and merge, with volumes
  conserved (2026-10-04, macOS Debug).
- Headless macOS Release on Bistro (2026-10-04): a doorway carved through a
  wall, a hollowed block, an extruded and face-moved step, a merge, a clip
  keeping both pieces and a refused face move that would break the solid. A
  defect course got one issue each for a 0.62 m step, a 51° ramp, 1.25 m of
  headroom, a 0.37 m gap, two overlapping brushes and an unreachable island;
  `query.reachable` found a 38.7 m route and refused the island. In the
  Scene, `ui.click ... alt` selected a face, `ui.key alt+up` and a handle drag
  moved it by 0.25 m and 2.75 m, the clip tool split a brush, and the Level
  checks window listed and marked the course's issues.
- `./build_test.sh` suite `io` covers delivery order, delay deadlines, fire
  limits, a stale target after its slot was reused, the zero-delay chain
  fault, counters with a built-in input, timers and publication problems;
  suite `scene_physics` covers generated bodies stepping alone and
  surviving a reset; suite `character` covers a sensor reporting a
  character (2026-10-04, macOS Debug).
- Headless macOS Release on Bistro (2026-10-04): one batch made a trigger
  brush with `trigger`, a brush with the sample `door`, a Player Start
  inside the trigger and an `io.connect` to the door's `open`. On
  `sim.play` the spawned player's capsule entered the trigger and the door
  rose 2.5 m and the trace printed
  `[io] 0.017 Lobby trigger.on_enter(Player) -> Door A.open`; `io.fire` sent
  `close` and it returned. In Details, a trigger listed a routing and a
  broken connection, Pick target and the Scene set the broken one's target,
  `+` made and selected a new connection, the door listed both incoming
  connections, and `level.lint` reported the broken one.
- `./build_test.sh` suite `heightfield` covers a file round trip with a
  partial last tile, the brush, flatten, ramp and paint results at named
  samples, the touched rectangle an operation predicts, and a stroke folded
  into one undo entry that undo and redo restore; suite `physics` covers a
  height field body (2026-10-04, macOS Debug).
- Headless macOS Release on Bistro (2026-10-04): `terrain.create`,
  `terrain.brush`, `terrain.flatten`, `terrain.ramp`, `terrain.sample` and
  `scene.describe` on a 256 m terrain north of the town; `query.raycast`
  hit the sculpted ground through the height field; a capture showed the
  tiles. With the Terrain window's tool on, a `ui.drag` stroke raised a band
  along its path, and one `undo` restored every sample to -0.5 m.
- `./build_test.sh` suite `material_pbr` covers a terrain material's
  composition: layer 0's factors with blending and transmission removed,
  each later layer's factors, a white unnamed layer, each map streaming
  into its own layer slot, and a missing layer file failing (2026-10-04,
  macOS Debug).
- Headless macOS Release on Bistro (2026-10-04): a 256 m terrain painted
  with layers 2, 3 and 4 shows the floor, orange and blue dev colors where
  painted in lit and unlit captures. The same run under Metal API validation
  reports no diagnostics. The four Vulkan G-buffer resolve modules and six
  transmission modules pass `spirv-val --target-env vulkan1.4
  --scalar-block-layout`. Metal's startup reflection accepted its terrain
  row; the Vulkan reflection check and native Vulkan execution are
  unverified.
- `./build_test.sh` suite `spline` covers a straight curve's exact length,
  spacing and last sample, and a closed curve through eight points of a
  circle: it passes through each point, stays within 1.5% of the radius
  between them, and measures the circumference within 0.4 m; suite
  `heightfield` covers a road following its centreline's heights inside
  its width and leaving ground past its falloff (2026-10-04, macOS Debug).
- Headless macOS Release on Bistro (2026-10-04): on a 256 m hilly terrain,
  `spline.create` with a `spline_mesh` of Bistro's trash can placed 23
  copies along a 174 m spline; `terrain.road` cut a 6 m road through a hill
  that the terrain samples at 1.05 m and 1.47 m where the spline is 1.0 m
  and 1.5 m high, leaving the hilltop at 9.5 m; `scatter.create` dropped 30
  copies of a Bistro tree mesh onto the hills. A capture shows the upright
  copies, the road, the trees on the hills and the spline's curve.
- Indicative cost, not a harness claim: the headless Release editor on an
  M1 Pro (MacBookPro18,3) with Bistro in view rendered a median frame of
  8.72 ms (p95 9.07 ms) before and 9.14 ms (p95 9.40 ms) after adding 1,000
  visible box brushes, from the editor's `stats.frame_ms` over 120 frames.
- Windows and native Vulkan, 2026-10-04 (RX 6700 XT, headless editor driven
  by `--exec` and in-process `op`): the CPU suites, terrain sculpt and paint,
  proxies, rebase and large terrain files pass; the
  [Windows record](../proposals/windows-vulkan-verification.md) lists the
  fixes this needed and the open items. The socket and `vkr_mcp` were
  unavailable on Windows then.
- Windows and native Vulkan, 2026-10-06 (headless Release editor, Bistro):
  `brush.stairs` built a 300 m spiral of 1,500 steps as one shape whose
  collision a downward ray hits at 97.6, 197.6 and 249.6 m; `blockout.build`
  raised it to 600 m in one undo step (a hit at 549.6 m) and `undo` brought
  back 300 m; `blockout.bake` refused it. Tiles cut from a corridor's wall
  let a ray through to the far wall. `brush.reshape` and `brush.clip` kept
  the brush's id and its `free_placement`, and two `undo` restored the box.
  A room and stairs parented onto a 1 m slab rest with floors at its top
  (1 m). `mover.create` (+Y, 3 m) and `io.fire` of `open` in a session
  without scripts raised the door to 3..6 m, a ray through the doorway
  missed, and Stop returned it to its saved pose. Suite `brush` covers the
  layout's capacity, a 4,000-step spiral and openings, and edge and corner
  dents; suite `heightfield` covers holes against Jolt; suites `io` and
  `scene_physics` cover movers and kinematic generated bodies; 684 passed
  (Windows Debug).
- Windows (2026-10-06, Windows 10 Pro 19045, headless Release editor on
  Bistro, temporary directory under a Cyrillic account name): the editor
  listened on `C:/Users/<name>/AppData/Local/Temp/vkr/editor-1001.sock`, and
  `vkr_mcp` without `--socket` resolved the same path. It answered
  `server/discover`, listed 65 tools, ran `vkr_cmd` `wait.scene` and
  `vkr_editor_status` (scene loaded, 5,989 entities), and `initialize`
  returned -32022. An explicit `--agent-socket` path behaved the same, and
  the socket file was removed on exit. Suite `local_socket` covers listen,
  nonblocking accept, a 1.5 MiB line, peer close, stale and live socket
  files, the wake pair, a non-ASCII directory and the path limit.
- Windows and native Vulkan, 2026-10-05 (RX 6700 XT, headless Release
  editor, Bistro): `./build_test.bat` suite `brush` covers a cube's hull, a
  raised edge (1.5 m³), a grid-line bend split into two 1.25 m³ pieces with
  a new shared face, a refused dent, a corner off the brush, a pulled cell
  (1 m³, no merge), a pushed recess (16 - 0.5 m³) and a whole pulled top
  merged into one 32 m³ brush. Through `op`, a pulled cell made a new brush,
  a pulled whole top merged, a push carved parts, a grid-line bend split a
  brush in two, an edge raise reshaped one, and a dent was refused with its
  reason. In Level Design's Scene a 4 x 2 x 4 m box showed its top and front
  grids; a cell drag selected a 2 x 3 patch whose arrow pulled a new brush
  up (`undo` removed it), a grid-line drag made a ridge, and an edge drag
  raised the front top edge. No Metal run.
- Windows and native Vulkan, 2026-10-06 (RX 6700 XT, headless Release
  editor on Bistro, driven by `vkr_mcp` clients from a Python script): after
  `terrain.brush` raised a terrain by 4 m, a raycast with `settle` false hit
  the old ground at y 0.00 and the default raycast, 82 ms later, hit
  y 3.99; a moved brush was missed with `settle` false and hit at its 3 m
  top by default; `brush.box` and `terrain.create` with `settle` answered
  `built` and `loaded` with their bounds; `terrain.sample` returned a 5 by 5
  grid; `level.map` drew a room's walls, its floor, a crate out of reach and
  the start; an agent's `undo` was refused with `VKR-AGENT-0009` over
  another agent's step, whose own `undo` ran; `changes.list` and the Changes
  window named both authors and labels; 48 `editor.status` calls took
  0.58 s from one client and 0.14 s spread over four; and an eighth client
  was served while a ninth received the refusal line. Release and Debug
  editor builds and `./build_test.bat` (685 passed) succeeded. No Metal run.
- Windows and native Vulkan, 2026-10-06 (RX 6700 XT, headless Release
  editor on Bistro, three `vkr_mcp` clients): alpha claimed a 30 m yard and
  beta's overlapping claim was refused while a touching one succeeded;
  beta's brush inside the yard was reverted and no longer existed, beta's
  edit of alpha's crate was refused before it applied, beta's move of its
  own box into the yard was reverted with the box at its old place, and
  alpha's move of beta's box into beta's claim was reverted; a terrain raise
  in a claim was refused and one outside applied. `changes.feed` listed the
  claims and batches of both authors with the crate's 4 x 3 x 4 m box, and
  reading on from `next` returned nothing. `entity.place` set a 1 m box on a
  room's floor at its centre (1114, 0, 1113), on a crate's top at
  (1102, 3, 1102) and 0.5 m beside its +x side. The Changes window listed
  both authors' changes and claims, and its Release claim freed alpha's.
