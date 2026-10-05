---
status: partial
updated: 2026-10-04
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

### Brush editing

Editing operations replace brushes in one journal group, keeping each
derived face's material and texture settings from the face it copies
([vkr_brush.c](../../runtime/src/level/vkr_brush.c), `VkrBrushPiece`):

| Operation | Result |
|---|---|
| `brush.move_face` | Moves one face (`face`, or `brush` and `side`) along its normal by `distance`; refused when the solid would break |
| `brush.extrude` | A new brush grown out of a face by `distance` |
| `brush.clip` | Cuts a brush with the world plane through `point` facing `normal`, keeping `back`, `front` or `both` pieces |
| `brush.hollow` | Replaces a brush with walls of `thickness` around its inside |
| `brush.carve` | Subtracts a `cutter` from `target`, or from every brush it touches, as non-overlapping convex pieces, at most one per cutter face; deletes the cutter unless `keep_cutter` |
| `brush.merge` | Joins 2 to 8 brushes into one when their union is convex: the merged solid's volume must equal the sum of theirs |

Carving keeps no boolean tree: pieces are ordinary brushes. Operations work
on unscaled brushes, since a scaled brush's planes are not the planes the
designer sees.

In the Scene, Alt+click selects the brush face under the pointer instead of
its object. A selected face shows its outline and a move handle along its
normal: a drag moves the face in 0.25 m steps, and Alt+Up or Alt+Down moves
it 0.25 m out or in (1 m with Shift). The clip tool (View > Clip brushes,
Cmd `brush.clip_tool`) cuts the selected brush with the vertical plane
through two clicks on the grid plane and keeps both pieces. All three submit
the operations above through the agent queue, so they undo like agent work.

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
reachable with one route and its length.

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
bar, and `level.lint` reports connections that will not route as
`broken_connection`.

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
| `terrain.create` | A terrain of `size` metres at `spacing` (default 256 m at 1 m), written to `assets/terrain/<uuid>.vkrhf` |
| `terrain.brush` | Raise, lower, smooth, flatten or paint a layer with a round brush at up to 256 points |
| `terrain.flatten` | Level a footprint at a height, blending over a falloff |
| `terrain.ramp` | A straight slope of a width between two surface points |
| `terrain.stamp` | Add or set heights from a grayscale PNG over a square |
| `terrain.sample` | Ground heights at x and z points |

In the editor ([editor_terrain.c](../../editor/src/editor_terrain.c)), the
Terrain window holds the sculpt tool and its mode, radius, strength and
paint layer. While the tool is on, the Scene draws the brush circle where
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

`terrain.create` writes the heightfield file at once, so undoing it or
leaving the scene unsaved leaves the file behind. A resident terrain keeps
all its samples: the largest, 1,025 samples a side, holds 6 MiB of samples
and 256 tile geometries. The owner chose all-resident terrain up to 1 km at
1 m spacing; larger terrains stream by tiles
([ADR-086](086-world-partition.md)). Imported, cooked terrain
meshes were declined in favour of heightfields built in the scene.

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
  fixes this needed and the open items. The socket and `vkr_mcp` remain
  unavailable on Windows.
