---
status: proposed
updated: 2026-10-04
authority: proposal
---
# Level design toolkit

A level design toolkit for the editor that designers and LLM agents use
together. Architecture and blockout use convex brushes in the style of Source's
Hammer, TrenchBroom and Chisel's Rockwall 2. Outdoor ground uses a heightfield
terrain later. Every tool is one typed operation that the viewport, the Cmd bar
and an agent socket call, so validation and undo are the same for each caller.

## Settled decisions

The owner settled these on 2026-10-04:

| Decision | Choice | Rejected alternatives |
|---|---|---|
| Geometry for architecture | Convex brushes. Carve splits brushes once and keeps no boolean tree. | Live CSG booleans (UE BSP, Godot CSG); editable meshes (ProBuilder, UE Modeling Mode) |
| First target | Indoor blockout with brushes; terrain follows | Terrain first; both tracks in parallel |
| Agent connection | A local newline-delimited JSON socket in the editor, with an MCP adapter | Extending only the Cmd bar; a built-in chat panel tied to one LLM provider |
| MCP revision | Only the 2026-07-28 revision, the newest stable release on 2026-10-04. A client must send 2026-07-28 requests to connect | Also answering the 2025-11-25 `initialize` handshake for older clients |
| Adapter location | A C executable in `tools/` that the build wrappers produce beside the editor. It implements the small stdio subset of MCP that it needs, so it needs no Node or Python SDK at run time | An adapter written on an MCP SDK in another language |
| Agent edit review | A pending changeset by default; direct application as an option | Direct edits with undo only |
| Triggers and IO | Source-style outputs, inputs and connections, declared by engine components and by C script components through `sdk.h` | IO as a separate system that scripts cannot extend |
| Visual scripting | Later work. IO and its value kinds must let a future graph asset, in the style of Unreal Blueprints, use the same router | Designing the graph editor now |
| Brush storage | One entity per brush, under group entities that the Outliner collapses; each face is a child entity, because a component cannot hold a face list | One component that holds many brushes; a new array property kind |
| Brush limits | 64 faces per brush; generators split anything larger. If measurement requires merging, cells start at 32 m | Unbounded faces |
| Changeset persistence | In memory only; closing the scene prompts before it discards pending items | Saving pending items with the scene overlay |
| Invalid connections | Report and skip, so a level plays with one broken connection | Refusing Play until every connection resolves |
| Connection targets | An entity in the source's own container, because entity references never cross containers (ADR-076). Named targets across containers can come later | Cross-container references now |
| IO timing | Routing in fixed ticks, so delays and order do not depend on the frame rate | Routing once per rendered frame |

## Current baseline

- **Text control.** The Cmd bar, `--exec` and `--headless` drive the editor by
  text and print `[cmd]` lines
  ([ADR-075](../adr/075-editor-cmd-bar-and-evaluator.md),
  [editor_cmd.c](../../editor/src/editor_cmd.c)). A line holds at most 255
  bytes, one statement runs per frame, a statement makes at most one scene
  edit, and expressions cannot create entities. Results are text, and the
  editor cannot return an image.
- **Object model.** Entities are an ID plus components. Type descriptors drive
  Details, JSON, validation, undo and Cmd paths, and modules register new
  component types ([ADR-076](../adr/076-project-object-model.md),
  [vkr_type_desc.h](../../runtime/src/core/vkr_type_desc.h)).
- **Edit journal.** [vkr_scene_edit.h](../../runtime/src/renderer/systems/vkr_scene_edit.h)
  records apply, create, delete, reparent and component edits.
  `vkr_scene_edit_apply_gesture` merges consecutive edits of one entity and
  field set; `vkr_scene_edit_apply_physics_batch` groups physics changes. No
  entry groups arbitrary structural edits across entities into one undo step.
- **Generated geometry.** The `shape` component has one type, `cube`, and
  setting it rebuilds a generated mesh
  ([vkr_scene_types.c](../../runtime/src/renderer/systems/vkr_scene_types.c)).
  `vkr_geometry_system_create` accepts vertex and index data
  ([vkr_geometry_system.h](../../runtime/src/renderer/systems/vkr_geometry_system.h)).
- **Collision.** Colliders are boxes, spheres, capsules, convex hulls and
  triangle meshes; sensors exist; a body holds at most
  `VKR_PHYSICS_MAX_COLLIDERS` (32) colliders
  ([ADR-072](../adr/072-entity-collision-and-rigid-body-physics.md),
  [vkr_physics.h](../../runtime/src/physics/vkr_physics.h)). VKR does not wrap
  Jolt's height field shape.
- **Player metrics.** `VkrPhysicsCharacterDesc` holds the capsule radius and
  half-height, `max_slope_radians`, `step_up` and `step_down`.
- **Socket precedent.** `vkr_bakery serve` speaks newline-delimited JSON over a
  local stream socket
  ([vkr_bakery_serve.c](../../tools/bakery/vkr_bakery_serve.c)).
- **Prefabs.** `scene.instantiate` copies a project scene under a new root
  without a link to its source.
- **Sensor events.** Physics reports sensor begin and end pairs
  (`VkrPhysicsSensorEvent`), and the sample runtime drains them each frame
  ([vkr_sample_runtime.c](../../runtime/src/vkr_sample_runtime.c), in its
  scene update). No code reads the drained events, so scripts cannot react to
  a trigger.
- **Scripts.** C script modules declare components, fields, behaviors and
  module hooks through `sdk.h` macros
  ([ADR-079](../adr/079-c-script-modules.md), [sdk.h](../../sdk/sdk.h)).
  Structural edits in fixed ticks are queued and replay after the tick. The SDK
  has no event, message or trigger hook.
- **Descriptor limits.** Properties never describe dynamic arrays
  ([vkr_type_desc.h](../../runtime/src/core/vkr_type_desc.h)), and no property
  kind references an entity. A component cannot hold a list of connections or
  a field that points to another entity.
- **Not present.** Brushes, terrain ([Terrain rendering](terrain-rendering.md)
  is a proposal), splines, scatter and entity IO. The
  [behavior proposal](entity-behavior-system.md#second-deliverable-connections-and-constrained-state-charts)
  plans connection assets that bind a typed event to an action on an entity.

## Lessons from other engines

| Engine | Take | Leave |
|---|---|---|
| Source (Hammer) | Convex brushes, per-face materials with world-aligned UVs, brush entities such as triggers, inputs and outputs, a power-of-two grid | BSP, VIS, leak checks and lightmap compiles; VKR culls on the GPU and bakes diffuse volumes ([ADR-054](../adr/054-baked-diffuse-volumes.md)) |
| Chisel (Rockwall 2) | Confirms the Hammer model and Source-style IO for a new engine | Lightmaps and PVS, for the same reason |
| Unreal Engine 5 | Heightfield landscape with paint layers, splines, rule-based scatter (PCG) | Legacy BSP: an ordered boolean tree with slow, fragile rebuilds |
| Unity | Face extrude, inset and bevel (ProBuilder) as brush operations | Editable meshes as the source of architecture |
| Godot | Tile-kit snapping for modular props (GridMap) | CSG nodes, which Godot documents as a prototyping tool |

A brush is a few planes with a material on each face. An agent can read and
write that as text, and grid snapping keeps the values exact. A mesh has no
compact form that an agent can edit reliably.

## Proposed design

### Authoring layers

| Layer | Holds | Compiles to |
|---|---|---|
| Brushes | Convex solids with per-face materials | One mesh and one convex collider per brush, merged per world cell if measurement requires it |
| Terrain | Heightfield tiles, weight layers, holes | Terrain tiles and height field collision |
| Placement | Props, splines, seeded scatter rules | Mesh instances |
| Gameplay | Brush entities, point entities, IO connections | Sensor bodies, movers and runtime connections |

The scene document stores authored data only. Generated meshes and colliders
are derived data that the scene rebuilds from that data when it loads or
changes, in the editor and in a packaged game.

### Brush data

A brush is an entity with a `brush` component. Each face of the brush is a
direct child entity with a `brush_face` component. A face holds one plane in
the brush's local space, a material reference (the `shape` component's
material name and path fields are the model), and UV offset, scale and
rotation. Faces are world-aligned by default, as in Hammer, so a resize keeps
texel density and neighbouring walls line up. Vertices snap to a power-of-two
grid in meters, from 0.0625 m to 8 m.

Faces are entities because a component value holds at most
`VKR_TYPE_VALUE_MAX` (1,024) bytes and descriptors cannot describe arrays
([vkr_type_desc.h](../../runtime/src/core/vkr_type_desc.h)). The same pattern
already stores colliders (ADR-072). Each face then gets Details rows, undo,
JSON, Cmd paths and agent operations from its descriptor, and selecting a face
is selecting an entity.

Validation at the face and brush boundary rejects non-finite planes, brushes
with fewer than 4 or more than 64 faces, faces that do not bound a polygon, and
open or zero-volume solids. Mesh and collider generation then consume only
valid brushes.

Generators emit ordinary brushes: box, wedge, cylinder, cone, stairs and arch.
Editing operations are face moves, vertex moves, clip by a plane, hollow, face
extrude, merge and carve. Carve replaces the target with convex pieces once
and records one undo step.

### Brush meshes and collision

Each valid brush owns one generated mesh with one submesh per distinct face
material, as the `shape` component owns its cube. A brush rebuilds at most once
per frame, after every edit of that frame has applied. A solid brush owns a
static physics body with one convex hull collider built from its vertices.

The phase 1 evidence measures the frame cost of 1,000 brushes placed in the
Bistro scene. Per-brush meshes stay when that cost is within the per-draw cost
of Bistro's own static meshes. Otherwise the scene merges brushes per 32 m world
cell and per material, removes faces hidden between touching solid brushes,
and resolves picks inside a cell on the CPU against brush planes.

Export to glTF hands a blockout to an artist. Replace with mesh swaps a brush
group for the finished model and keeps its brushes as collision only.

### Operation layer

Each tool is a named operation with typed, bounded arguments, for example
`brush.create`, `brush.clip`, `face.set_material`, `blockout.room`,
`io.connect` and `query.raycast`. One table defines its name, arguments,
validation, result and journal entry. The viewport tools, the Cmd bar and the
agent socket call the same table. The Cmd bar keeps its current commands; new
toolkit commands are operations exposed through it.

A batch is one compound journal entry: one undo step that may create, edit and
delete several entities. This entry is new journal work. A `dry_run` flag
validates a batch without changing the scene.

Intent operations let an agent state what it wants while the engine computes
geometry: `blockout.room`, `blockout.corridor`, `blockout.doorway` and
`blockout.stairs(from, to)`. Each emits ordinary brushes that a designer edits
afterwards.

### Agent channel

The editor listens on a per-user local socket (a Unix socket with mode 0600, a
named pipe on Windows) and speaks newline-delimited JSON. A request carries an
ID, an operation or a batch, and optional `changeset` and `dry_run` fields. A
response carries the same ID, created entity IDs, results or an error code with
a message. It never listens on TCP.

A small adapter executable translates MCP over stdio to that socket, so Claude
Code or any MCP client connects without code in the editor that knows about a
model provider. It implements only the
[2026-07-28 revision](https://modelcontextprotocol.io/specification/2026-07-28/changelog),
which the SDKs label v2:

- **Stateless requests.** The revision has no `initialize` handshake. Each
  request carries its protocol version and client capabilities in `_meta`, and
  the adapter answers `server/discover` with its versions, capabilities and
  identity. An unsupported version returns `UnsupportedProtocolVersionError`.
- **Handles as arguments.** The protocol keeps no session. The changeset ID,
  entity IDs and capture IDs are server-minted handles that tools take as
  ordinary arguments, so any request can name the changeset it extends.
- **Tools.** `tools/list` returns the operation table in a fixed order with
  `ttlMs` and `cacheScope`. Results set `resultType` to `complete`, return JSON
  as `structuredContent` with an `outputSchema`, and return captures as image
  content.
- **Long operations.** A scene load, bake or build reports progress on its
  request. The tasks extension (`io.modelcontextprotocol/tasks`) is optional
  for work that outlives one request.
- **Not used.** The adapter implements none of the deprecated Roots, Sampling
  and Logging features. It logs to stderr.

The editor socket protocol stays VKR's own and independent of MCP revisions;
only the adapter changes when MCP changes.

Perception operations:

- `level.describe(region)` returns rooms, openings, materials, entities and
  connections in a compact form.
- `query.raycast`, `query.bounds` and `query.at(point)` answer spatial
  questions.
- `view.capture` returns a PNG of a top-down orthographic map with grid labels,
  or a camera view. It renders through the offscreen present target
  ([ADR-014](../adr/014-offscreen-present-target.md)).

### Pending changeset

By default, agent operations land in a pending changeset instead of the edit
journal. The viewport draws pending items as ghosts, and a Changes panel lists
them. Accept moves the chosen items into the journal as one undo step; Reject
drops them. Validation runs when the item enters the changeset and again on
accept, because the scene can change in between. A request may ask for direct
application when the designer allows it.

### Level checks

`level.lint` checks a region against the player character's capsule and limits:
openings narrower than the capsule, ceilings lower than its height, steps
higher than `step_up`, slopes steeper than `max_slope_radians`, unreachable
spawns, overlapping solid brushes and open edges into the void.
`query.reachable(a, b)` answers one path question. The same checks appear in an
editor panel for designers.

### Gameplay volumes

A brush entity is a brush group with a role: trigger, player clip, volume or
mover. A trigger builds a sensor body. Point entities include the existing
Player Start, spawns and markers. Until brushes exist, a `trigger` uses the
entity's existing box, sphere or capsule sensor collider, so IO does not wait
for the brush phases.

### Triggers, IO and scripts

IO uses three terms from Source:

| Term | Meaning | Example |
|---|---|---|
| Output | A named fact that a component fires | `trigger.on_enter`, `door.opened` |
| Input | A named action that a component accepts and validates | `door.open`, `trigger.disable` |
| Connection | Output to target input, with a value, a delay and a fire limit | `on_enter → door_a.open, delay 0.5 s, once` |

An input is a request: the target's handler validates its own state, so a
connection cannot open a locked door. An output is a fact in the behavior
proposal's sense; a receiver cannot veto it.

**Declarations.** Engine components declare their outputs and inputs in their
descriptors:

| Component | Outputs | Inputs |
|---|---|---|
| `trigger` | `on_enter`, `on_exit`, `on_empty` | `enable`, `disable`, `toggle` |
| `relay` | `on_trigger` | `trigger`, `enable`, `disable` |
| `timer` | `on_timer` | `start`, `stop`, `set_interval` |
| `counter` | `on_changed`, `on_max`, `on_min` | `add`, `subtract`, `set` |
| Every placed entity | none | `show`, `hide`, `destroy` |

A trigger filters by collision mask and, optionally, by a component type that
the other entity must carry, such as the player's. `on_empty` fires when the
last filtered entity leaves.

Script components declare outputs and inputs next to `VKR_COMPONENT`, so a
script joins the same wiring as engine components (pseudocode; the macro
names are proposed):

```c
VKR_OUTPUTS(door,
            VKR_OUTPUT(opened, "On opened")
            VKR_OUTPUT(closed, "On closed"))

static void door_open(VkrCtx *ctx, VkrEntity self, Door *door,
                      const VkrIoValue *value) {
  if (door->locked) {
    return;
  }

  door->target_angle = door->open_angle;
  vkr_io_fire(ctx, self, door_output_opened, NULL);
}

VKR_INPUTS(door,
           VKR_INPUT(open, "Open", NONE, door_open)
           VKR_INPUT(close, "Close", NONE, door_close))

VKR_BEHAVIOR(door, .update = door_update, .inputs = door_inputs,
             .outputs = door_outputs)
```

Scripts also reach IO from code:

- `vkr_io_fire(ctx, self, output, value)` fires one of the script's outputs.
- `vkr_io_send(ctx, target, input, value)` calls an input on another entity
  through the router, as Source's `ent_fire` does. The input identifier comes
  from `vkr_io_input(ctx, type, "open")`, resolved once in `start`.
- Behaviors on a trigger entity get `trigger_enter` and `trigger_exit` hooks
  with the other entity, as Unity's `OnTriggerEnter` does. A script then
  reacts to its own volume without a connection.

Value kinds are `NONE` and the `VKR_FIELD` kinds, plus a new `ENTITY` kind.

**Storage.** Each connection is a child entity of its source with one
`io_connection` component, as each collider is a child entity of its body
owner (ADR-072). Its fields are the output name, the target, the input name,
the value, the delay in seconds and the fire limit (0 for unlimited). Child
entities give each connection Details rows, undo, JSON, Cmd paths and agent
operations without array support in descriptors. The Outliner hides them; the
owner's Details shows them.

The target needs a new `ENTITY` property kind that stores a document-stable
entity ID ([ADR-076](../adr/076-project-object-model.md)) and resolves to a
generational `VkrEntity` at load. The same kind gives script fields entity
references, such as a button that names its door.

**Routing.** An IO router in the script host owns delivery for the simulated
scene. It becomes the single physics-event drain from the behavior proposal:
the unused sensor drain in the sample runtime moves into it.

1. **Publication.** At Play start and at container load during a session, the
   router resolves output and input names to per-type indices, targets to
   `VkrEntity` handles, and value kinds against the input. It builds a sparse
   table keyed by source entity and output. An invalid connection is reported
   in the Console and in Details and never routes; Play continues. A
   zero-delay cycle is reported as a warning.
2. **Collection.** In `after_tick`, the router reads sensor events, applies
   trigger filters and adds the fired outputs in producer order. Outputs that
   scripts fired during the tick follow in their sequence order.
3. **Delivery.** Zero-delay deliveries run first-in, first-out in the same
   `after_tick`, where input handlers may spawn and destroy directly. A
   delayed delivery waits in a deadline queue in simulation time and joins the
   queue of the first tick at or after its deadline.
4. **Bounds.** The router reserves its queues at session start. A per-tick
   delivery limit and a zero-delay chain depth limit apply. Exceeding either
   faults the session with the chain that caused it, as the behavior proposal
   requires; it never drops or defers a delivery silently.
5. **Lifetime.** A delivery carries the target's generation. A destroyed
   target drops the delivery and increments a counter. Stop and Reset clear
   pending deliveries and fire counts.

Ordering by tick, phase, producer order and sequence makes a session
replayable from the same inputs.

**Editor and agents.** Details shows an Outputs section with the entity's
connections: an output list from its components, a target picked in the Scene
or Outliner, an input list from the target's components, the value, delay and
fire limit. An Inputs section lists incoming connections and selects their
sources. The Scene draws lines from the selection to its targets and from its
sources. During Play an IO trace prints lines such as
`[io] 12.350 trigger_lobby.on_enter(player) -> door_a.open`. The operations
`io.connect`, `io.disconnect`, `io.list`, `io.fire <entity> <input> [value]`
and `io.trace` serve the Cmd bar and agents, and `level.lint` reports
connections with a missing target or input.

**Visual scripting later.** IO wires entities together, as Source IO and the
event links of Unreal's Level Blueprint do. A future graph asset, in the style
of Unreal Blueprints, is a script component whose entry nodes are its inputs
and whose fire nodes are its outputs. It uses the same router and connection
data without migration, and its pin kinds are the IO value kinds. The behavior
proposal's state charts consume the same facts.

### Terrain and population

Terrain follows the open decisions in [Terrain rendering](terrain-rendering.md)
and adds sculpt and paint tools, holes for brush-built entrances, and height
field collision. Agent operations work on regions, not strokes:
`terrain.flatten(footprint)`, `terrain.ramp(a, b, width)`,
`terrain.stamp(heightmap)` and `terrain.road(spline)`. Population adds splines
that repeat or bend meshes, seeded scatter that a designer can re-roll, and
linked prefabs once the behavior proposal defines their lifecycle.

## Specification

This section is the implementation contract. Phases 0 and 1 are complete;
later phases state their interfaces and gain detail when they start.

### Phase 0: agent channel

**Owners.** `editor/src/editor_agent.c` owns the socket, connections and the
request lifecycle. `editor/src/editor_ops.c` owns the operation table and runs
operations on the UI thread during the editor's UI build. The runtime applies
their scene edits after that build, as it applies every editor request. No
operation runs on another thread, so the operation table needs no locks.

**Socket.** On macOS and Linux the editor creates the directory
`$TMPDIR/vkr` (`/tmp/vkr` without `TMPDIR`) with mode 0700 and listens on
`editor-<uid>.sock` inside it with mode 0600. When a live editor already
listens there, the second editor listens on `editor-<uid>-<pid>.sock` and
prints that path in the Console. A stale socket file that refuses connection
is removed. `--agent-socket <path>` and `VKR_EDITOR_AGENT_SOCKET` select the
path; `--no-agent-socket` disables the listener. The editor polls the
nonblocking socket once per UI frame, accepts at most 4 clients, and closes a
client that sends a line longer than 1 MiB. Windows support is pending; the
editor there reports the channel as unavailable.

**Messages.** Each message is one line of UTF-8 JSON. A request is
`{"v":1,"id":<number|string>,"op":"<name>","args":{...}}`. A response is
`{"v":1,"id":<same>,"ok":true,"result":{...}}` or
`{"v":1,"id":<same>,"ok":false,"error":{"code":"VKR-AGENT-NNNN","message":"..."}}`.
Requests from all clients run first in, first out, one at a time. A request
whose scene edits apply after the UI build, or whose capture renders later,
answers on a later frame.

| Code | Meaning |
|---|---|
| `VKR-AGENT-0001` | Malformed JSON or message shape |
| `VKR-AGENT-0002` | Unknown operation |
| `VKR-AGENT-0003` | Invalid or missing argument |
| `VKR-AGENT-0004` | Entity or component not found, or a name is ambiguous |
| `VKR-AGENT-0005` | The scene rejected an edit; the batch rolled back |
| `VKR-AGENT-0006` | No scene is loaded, or the editor is busy with a load or Play |
| `VKR-AGENT-0007` | Capture failed |
| `VKR-AGENT-0008` | A limit was exceeded |

**Entity references.** Results name an entity as
`"<world>:<index>:<generation>"`. An argument that names an entity accepts
that form, an exact entity name (an ambiguous name fails), or `"$k"` for the
entity that operation `k` of the same batch created.

**Operations.**

| Operation | Arguments | Result |
|---|---|---|
| `ops.list` | | Every operation with its description and a JSON Schema for its arguments |
| `editor.status` | | Project, loaded containers, selection, simulation state, pending changes |
| `scene.describe` | `container`, `root`, `region` (`min`, `max`), `offset`, `limit` (at most 500) | Entities with ID, name, parent, component types, local transform and world bounds; brushes summarised with their face count and materials |
| `entity.get` | `entity` | Transform, visibility and every component as named property values |
| `entity.create` | `name`, `parent`, `container`, `position`, `rotation` (degrees XYZ), `scale`, `component` with `type` and `values` | Created entity |
| `entity.set` | `entity`, `name`, `position`, `rotation`, `scale`, `visible` | |
| `entity.delete` | `entity` | |
| `entity.parent` | `entity`, `parent` (null for a root) | |
| `component.add`, `component.set`, `component.remove` | `entity`, `type`, `values` | |
| `batch` | `ops`, `review` (default true), `dry_run`, `label` | One result per operation; the change ID under review |
| `changes.list`, `changes.accept`, `changes.reject` | `change` (accept without one accepts all) | |
| `undo`, `redo` | | |
| `query.raycast` | `origin`, `direction`, `max_distance` | Physics hit: entity, collider, position, normal, distance |
| `query.bounds` | `entity` | World bounds of the entity and its descendants |
| `view.capture` | `view` (`current`, `perspective`, `top`, `left`, `right`, `bottom`), `focus` (entity or `min`/`max`), `grid_labels` | PNG path, width and height |
| `cmd` | `line` | Every `[cmd]` result line the statement printed |

`component.set` takes property names from the type descriptor, so every
component, including registered script components, accepts the same values
as Details and Cmd paths. Values use the descriptor kinds: numbers, booleans,
strings, enum names, and arrays of two to four numbers.

**Batches and journal groups.** Every write operation outside a batch runs as
a batch of one. A batch holds at most 256 operations and all of them edit one
container. The editor validates every operation's arguments before it submits
the batch. The runtime opens a journal group, applies the edits in order,
resolves `$k` references, and closes the group. When an edit fails, the
runtime undoes the group's applied entries, drops them, and reports the
failing operation with the journal status. `dry_run` validates without
submitting.

`VkrSceneEditEntry` gains a `group` field. Undo and redo move over every entry
of a group as one step. `VKR_SCENE_EDIT_UNDO_CAPACITY` rises from 128 to 1,024
entries; eviction at capacity removes a whole group; a group that would exceed
512 entries fails and rolls back.

**Review.** A batch with `review` true becomes a pending change: its journal
group, its container, its label and the entities it created or touched. The
Changes panel lists pending changes with Accept, Reject and Focus, and the
Scene outlines their entities. Accept removes the pending mark only. Reject
undoes the group:

- when the group is the newest content of its journal, as an ordinary grouped
  undo that also drops the redo entries above it;
- otherwise, only when no later entry in that journal touches an entity of the
  group or a current descendant of one. The group's entries then undo in
  reverse order and leave the journal.

Reject otherwise fails and names the later edit. Pending marks live in memory.
The edits themselves are ordinary scene edits, so the existing unsaved-edits
prompt covers closing the scene, and closing it clears the marks.

**Captures.** `view.capture` optionally switches the view and frames the
focus, waits three frames, and captures `final_color`
([vkr_capture.c](../../renderer/src/vkr_capture.c)) cropped to the Scene
viewport, so grid labels and icons appear. The editor writes the PNG to
`$TMPDIR/vkr/captures/` and keeps the newest 32 files. It restores the
previous view and grid labels afterwards.

**MCP adapter.** `tools/agent/vkr_mcp.c` builds `vkr_mcp`. It speaks MCP
2026-07-28 over stdio, one JSON-RPC message per line, and connects to the
editor socket (`--socket`, `VKR_EDITOR_AGENT_SOCKET`, or the default path).

- `server/discover` returns the supported version `2026-07-28`, the `tools`
  capability and the server identity.
- Each request must carry `io.modelcontextprotocol/protocolVersion` equal to
  `2026-07-28`; another value returns `UnsupportedProtocolVersion` (-32022)
  with the supported versions.
- `tools/list` maps each operation from `ops.list` to the tool
  `vkr_<operation with dots as underscores>`, in table order, with
  `ttlMs` 60000 and `cacheScope` `private`.
- `tools/call` forwards the arguments. A result returns `structuredContent`
  and the same JSON as text; an editor error returns `isError`. A capture also
  returns the PNG as image content.
- Every result carries `resultType` `complete` and the server identity in
  `_meta`. Logs go to stderr.

**Phase 0 evidence.** CPU tests for journal groups (grouped undo and redo,
rollback, eviction of a whole group, out-of-order reject and its refusal) and
for message parsing. A headless Bistro editor run drives the socket through
`vkr_mcp`: create, set, batch with a `$k` reference, reject, capture and undo,
with the scene state checked through `entity.get`.

### Phase 1: brushes

**Components.** `brush` (category Level) holds `role`: `solid` (renders and
collides), `visual` (renders only), `clip` (collides only) and `trigger`
(sensor body; renders only in the editor). `brush_face` holds `normal` (unit,
brush local space), `distance` (the plane is `dot(normal, p) = distance`),
`material` (a material path, empty for the default), `uv_offset` and
`uv_scale` (meters per texture repeat), `uv_rotation`, and `uv_world`
(true projects in world space, false in brush space). Face entities have an
identity transform. The editor refuses transform edits on them and the
Outliner hides them under their brush.

**Geometry.** `runtime/src/level/vkr_brush.c` builds a brush's polygons on the
CPU: for each face it clips a large square on its plane by every other plane,
then welds vertices within 1e-4 m. It rejects a face whose polygon is empty, a
brush whose polygons do not close, and a brush with zero volume. UVs project
each vertex onto the two texture axes of the face's dominant normal axis, as
Hammer does, rotated by `uv_rotation`, divided by `uv_scale` and shifted by
`uv_offset`. Tangents follow the U axis.

**Scene integration.** A change to a `brush` or `brush_face` component, a
face's creation or deletion, or a brush transform change marks the brush
dirty. Scene update rebuilds each dirty brush once: one geometry with one
submesh per distinct material, owned and released like the shape mesh. The
build of an invalid brush keeps no mesh and reports the reason in Details.
Roles `solid` and `clip` own a static body whose one collider is a convex hull
of the brush's vertices, built in memory without a cooked asset.

**Operations.** `brush.box` (`min`, `max`), `brush.wedge` (`min`, `max`,
`slope` toward `+x`, `-x`, `+z` or `-z`), `brush.cylinder` (`center`,
`radius`, `height`, `sides` 3 to 32), `brush.stairs` (`from`, `to`, `width`,
`step_height`) and `brush.set_material` (`brush`, `material`, `faces`). Each
creator also takes `name`, `parent`, `role` and `material` and snaps its
corners to the current grid unless `snap` is false. Blockout operations emit a
group entity with brushes: `blockout.room` (`min`, `size`, `wall`, `material`,
`floor_material`, `ceiling`), `blockout.corridor` (`from`, `to`, `width`,
`height`, `wall`) and `blockout.doorway` (`wall`, `offset`, `width`,
`height`), which splits a box wall into the pieces around the opening.

**Editor.** The Create menu and Cmd `create` gain Brush box, Brush wedge and
Brush cylinder. A box drag tool in the Scene draws a brush on the grid plane
between two clicked corners, with the height from the grid spacing.

**Phase 1 evidence.** CPU tests for polygon building, rejection of open and
zero-volume brushes, UV projection, and the blockout generators' brush
counts and bounds. A Bistro editor run that builds a room and walks the
player through its doorway. Release frame cost on Bistro with 1,000 brushes,
against Bistro without them, which decides the cell merge.

### Phase 2: brush editing and level checks

Operations `brush.clip` (plane), `brush.hollow` (`thickness`), `brush.carve`
(`cutter` brush), `brush.extrude` (`face`, `distance`), `brush.move_face`
(`face`, `distance`) and `brush.merge` (brushes whose convex hull loses no
volume). Scene tools: face selection, face move handles and the clip tool.
`level.lint` and `query.reachable` as described in [Level checks](#level-checks);
reachability flood-fills walkable samples at the capsule radius with the
character's `step_up` and `max_slope_radians`.

### Phase 3: IO

As described in [Triggers, IO and scripts](#triggers-io-and-scripts). The
`trigger` component may use a brush with role `trigger` or a box, sphere or
capsule sensor collider.

### Phase 4: terrain

A `terrain` component references a heightfield asset in the project: 16-bit
heights and four 8-bit layer weights per sample, with a sample spacing and a
height range. The scene builds tile meshes from it and one height field
collider. Sculpt and paint edits change sample regions; a journal entry holds
the region's samples before and after. Detailed design starts from the
[Terrain rendering](terrain-rendering.md) decisions.

### Phase 5: population

A `spline` component with `spline_point` child entities, a `spline_mesh`
component that repeats a mesh along a spline, and a `scatter` component that
places seeded mesh instances in a box over the surface below. Generated
instances are runtime data rebuilt from the seed; the document stores only
the rules. Linked prefabs stay with ADR-076 and the behavior proposal.

## Phases

| Phase | Scope | Acceptance evidence |
|---|---|---|
| 0. Agent channel | Operation table over current tools (`create`, transforms, components, prefabs, Content placement); compound journal entry; socket; MCP adapter; `level.describe`; `view.capture`; pending changeset | A CPU test that undoes and redoes a mixed batch; a headless Bistro run whose socket batch, capture and undo match the Cmd results |
| 1. Brush core | `brush` and `brush_face` components, box, wedge and cylinder, face materials and world UVs, grid, brush meshes and colliders, blockout operations | CPU tests for polygon building and convexity rejection; a brush block inside the Bistro scene walked by the player; Release frame cost on Bistro with 1,000 brushes before and after |
| 2. Brush editing | Vertex, edge and face edits, clip, hollow, carve, extrude, brush entities, `level.lint` | Lint tests against named defects (narrow door, high step, steep ramp) |
| 3. IO | `ENTITY` property kind, `io_connection`, the router with the sensor drain, `trigger`, `relay`, `timer`, `counter`, script `VKR_OUTPUTS` and `VKR_INPUTS`, trigger hooks, the IO trace and operations | CPU tests for delivery order, delay deadlines, fire limits, stale targets and the chain limit fault; a Bistro Play run in which a trigger opens a script door |
| 4. Terrain | Terrain-rendering decisions, sculpt and paint, height field collision, region operations | Defined by the terrain-rendering proposal |
| 5. Population | Splines, seeded scatter, linked prefabs | A residency bound for M1 under the 16 GB floor ([ADR-083](../adr/083-supported-hardware-matrix.md)) |

## Risks

- One draw per brush can cost too much. Phase 1 measures it on Bistro and adds the cell merge if it does.
- LLM agents make arithmetic and orientation errors in 3D. Intent operations,
  grid snapping, `level.lint`, labelled top-down captures and the review step
  each reduce this; none removes it.
- An MCP client that sends only 2025-11-25 or older requests cannot connect.
  The adapter answers it with `UnsupportedProtocolVersionError`, so the failure
  is explicit.
- A socket opens the editor to other local processes. The user-only socket mode
  limits access to the user's own processes, which can already edit the
  project files.

## Sources

- [Chisel features](https://chiselengine.com/features)
- [Valve Hammer Editor](https://developer.valvesoftware.com/wiki/Valve_Hammer_Editor)
- [Source inputs and outputs](https://developer.valvesoftware.com/wiki/Inputs_and_Outputs)
- [TrenchBroom manual](https://trenchbroom.github.io/manual/latest/)
- [Godot CSG tools](https://docs.godotengine.org/en/stable/tutorials/3d/csg_tools.html)
- [Jolt HeightFieldShape](https://jrouwe.github.io/JoltPhysics/class_height_field_shape.html)
- [Model Context Protocol](https://modelcontextprotocol.io/)
- [MCP 2026-07-28 changelog](https://modelcontextprotocol.io/specification/2026-07-28/changelog)
