---
status: partial
updated: 2026-10-04
authority: adr
---
# ADR-084: Agent channel and level design toolkit

## Status

Accepted (partial). The agent channel and brushes (phases 0 and 1 of the
[level design toolkit](../proposals/level-design-toolkit.md)) are
implemented. Brush editing, level checks, IO, terrain and population remain
in that proposal until they ship.

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
JSON ([editor_agent.c](../../editor/src/editor_agent.c)). On macOS and Linux it
creates `$TMPDIR/vkr` (`/tmp/vkr` without `TMPDIR`) with mode 0700, refuses a
directory another user owns or others can open, and binds
`editor-<uid>.sock` under a 0177 umask. When a live editor already answers
there, the next editor binds `editor-<uid>-<pid>.sock` and logs the path. A
socket file nobody answers is stale and is replaced. `--agent-socket <path>`
and `VKR_EDITOR_AGENT_SOCKET` choose the path; `--no-agent-socket` turns the
listener off. Windows has no listener yet and reports the channel as off.

The editor polls the nonblocking listener once per UI build, serves at most 4
clients, closes a client whose unfinished line passes 1 MiB, and queues at
most 64 requests. A request is
`{"v":1,"id":<number|string>,"op":"<name>","args":{...}}`; a response is
`{"v":1,"id":<same>,"ok":true,"result":{...}}` or
`{"v":1,"id":<same>,"ok":false,"error":{"code":"VKR-AGENT-NNNN","message":"..."}}`.
Requests from every client run one at a time in arrival order; a request whose
edits apply after the UI build, or whose capture renders later, answers in a
later build. A headless editor (ADR-075) stays open while a client is
connected or a request waits or runs, so a script only needs to outlast the
client's connection.

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

### Operations

One table in [editor_ops.c](../../editor/src/editor_ops.c) defines each
operation's name, description, JSON Schema and handler; `ops.list` returns it,
and the MCP adapter builds its tools from that list. Operations run on the UI
thread during the editor's build.

| Operation | Purpose |
|---|---|
| `ops.list`, `editor.status` | The table; loaded containers, selection, simulation, view and pending changes |
| `scene.describe`, `entity.get`, `query.bounds` | Entities with ID, name, parent, component types, local pose and world bounds; one entity's components as descriptor JSON |
| `entity.create`, `entity.set`, `entity.delete`, `entity.parent` | Structure and pose; delete with `recursive` deletes descendants first |
| `component.add`, `component.set`, `component.remove` | Component values by descriptor property name, partial for `set` |
| `batch` | Several write operations as one journal group |
| `changes.list`, `changes.accept`, `changes.reject` | Review of agent edits |
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

Every write runs as a batch: one `VkrSampleEditBatchRequest` of at most 256
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
inside one stops there, as between two entries. The journal holds 1,024
entries; eviction removes the oldest whole group, and a group past 512 entries
fails. `vkr_scene_edit_group_rollback` undoes and drops the open group.

### Review

A batch with `review` (the default) becomes a pending change: its group,
container, label and the entities it created or edited, held in memory by the
operation table. The Agent changes window (View menu, Cmd `window changes`)
lists pending changes with Focus, Reject and Accept, the Scene outlines their
entities' local bounds in orange through the editor's line overlay, and a
toast announces each new change. Accept removes the mark only. Reject calls
`vkr_scene_edit_group_revert`:

- an undone group only loses its redo entries;
- otherwise no later applied entry may name an entity the group created,
  deleted, edited or reparented, a current descendant of one, or a parent of
  one as its own parent; a later physics batch or collision-layer edit also
  refuses. The group's entries then revert in reverse order and leave the
  journal, and the redo entries above the cursor are dropped.

A refused reject names the conflicting entity. A change disappears when its
group leaves the journal, as after a scene reload or an undo followed by a new
edit.

### Captures

`view.capture` optionally switches the camera view and grid labels and frames
an entity's world bounds or a box (`VkrSampleViewRequest.frame_box`). After
four builds it asks the runtime for one `final_color` capture
(`VkrSampleCaptureRequest`). The runtime marks editor captures with the high
bit of the request id, lends the poll result to the next build and releases it
after that build; the harness owns the capture slot when it runs, and the
request then fails. The editor converts RGBA8, BGRA8 or half-float color to an
RGBA8 PNG of the Scene image rectangle, or of the whole window with `area`
`window`, writes it to `$TMPDIR/vkr/captures/`, keeps the newest 32, and
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
  instructions and the server identity;
- `tools/list` maps each operation to `vkr_<operation with dots as
  underscores>` in table order with `ttlMs` 60000 and `cacheScope` `private`;
- `tools/call` forwards the arguments to the editor and returns the result as
  `structuredContent` and as JSON text, an editor error with `isError`, and a
  capture's PNG also as image content;
- every result carries `resultType` `complete` and `serverInfo` in `_meta`;
  notifications are ignored and logs go to stderr.

The adapter reconnects once per call, so it survives an editor restart. It
links Bakery's JSON tree, which the `vkr_bakery_json` library now shares with
Bakery and the editor.

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
It also generates box, wedge and cylinder planes.

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

Operations `brush.box`, `brush.wedge`, `brush.cylinder`, `brush.stairs`
(solid steps of at most `step_height` under a group yawed from `from` toward
`to`), `brush.set_material` (faces by `top`, `bottom`, `sides`, `+x`, `-x`,
`+z`, `-z`), `blockout.room` (floor, ceiling and four walls around an
interior box), `blockout.corridor` (open ends, yawed from `from` toward `to`)
and `blockout.doorway` (an unscaled axis-aligned box wall becomes up to three
brushes around an opening) build ordinary brushes. Every operation snaps
corners to `grid` (1/16 m unless set; 0 turns snapping off) and validates the
solid before submission. `scene.describe` and `entity.get` summarise a
brush's role, face count, build status and materials and list faces only with
`faces`. `view.camera` places the perspective camera, and `view.capture`
accepts `eye` and `target`.

In the editor, the Create menu's Level group adds Brush Box, Brush Wedge,
Brush Cylinder and Blockout Room at the placement point. Brush drawing (B,
View > Draw brushes, Cmd `brush.draw`) takes the Scene's mouse: a left drag
on the grid plane outlines a box one grid cell high and the release creates
it; Escape cancels the drag, then ends drawing. Cmd `op <operation> [json]`
runs any operation of the table.

## Consequences

Agents and scripts reach every editor feature through typed operations, with
the Cmd vocabulary still available through `cmd`. One journal group per
request makes agent work undoable and reviewable as the unit the agent chose.
Reverting a group out of order is refused instead of guessed whenever a later
edit could depend on it. A capture needs a rendered frame, so it reports the
Scene as rendered, not what the editor shows while rendering is stopped.

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

Brush meshes are not merged: a level pays one draw per brush. A
measurement put that at about 0.4 µs per brush (see Evidence). Brush
collision follows the brush only after its transform rests, so a dragged
brush collides at its old place until the drag ends.

## Revisit when

A Windows listener is needed, an agent needs notifications pushed to it,
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
- Indicative cost, not a harness claim: the headless Release editor on an
  M1 Pro (MacBookPro18,3) with Bistro in view rendered a median frame of
  8.72 ms (p95 9.07 ms) before and 9.14 ms (p95 9.40 ms) after adding 1,000
  visible box brushes, from the editor's `stats.frame_ms` over 120 frames.
- Windows and native Vulkan are unverified.
