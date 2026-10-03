---
status: proposed
updated: 2026-10-04
authority: proposal
---
# Level design toolkit

The remaining phases of the level design toolkit. The agent channel and
brushes are implemented and recorded in
[ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md); this
proposal keeps brush editing and level checks, Source-style triggers and IO
that C scripts declare, heightfield terrain, and splines and scatter. Every
phase adds its operations to the ADR-084 operation table, so the viewport, the
Cmd bar and agents share them.

## Settled decisions

The owner settled these on 2026-10-04. Decisions of the implemented phases
(brushes, the agent channel, MCP 2026-07-28 only, the C adapter, review and
brush storage) are recorded in ADR-084.

| Decision | Choice | Rejected alternatives |
|---|---|---|
| First target | Indoor blockout with brushes; terrain follows | Terrain first; both tracks in parallel |
| Triggers and IO | Source-style outputs, inputs and connections, declared by engine components and by C script components through `sdk.h` | IO as a separate system that scripts cannot extend |
| Visual scripting | Later work. IO and its value kinds must let a future graph asset, in the style of Unreal Blueprints, use the same router | Designing the graph editor now |
| Invalid connections | Report and skip, so a level plays with one broken connection | Refusing Play until every connection resolves |
| Connection targets | An entity in the source's own container, because entity references never cross containers (ADR-076). Named targets across containers can come later | Cross-container references now |
| IO timing | Routing in fixed ticks, so delays and order do not depend on the frame rate | Routing once per rendered frame |

## Current baseline

- **Agent channel and brushes.** ADR-084: typed operations over a local
  socket and `vkr_mcp`, batches as journal groups with review, captures,
  brush components with generated meshes and generated collision bodies, and
  blockout operations.
- **Collision.** Colliders are boxes, spheres, capsules, convex hulls and
  triangle meshes; sensors exist; a body holds at most
  `VKR_PHYSICS_MAX_COLLIDERS` (32) colliders
  ([ADR-072](../adr/072-entity-collision-and-rigid-body-physics.md)). VKR does
  not wrap Jolt's height field shape.
- **Player metrics.** `VkrPhysicsCharacterDesc` holds the capsule radius and
  half-height, `max_slope_radians`, `step_up` and `step_down`
  ([vkr_physics.h](../../runtime/src/physics/vkr_physics.h)).
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
- **Not present.** Brush editing operations, level checks, terrain
  ([Terrain rendering](terrain-rendering.md) is a proposal), splines, scatter
  and entity IO. The
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
| Brushes (ADR-084) | Convex solids with per-face materials | One mesh per brush; convex hulls in static bodies per world cell |
| Terrain | Heightfield tiles, weight layers, holes | Terrain tiles and height field collision |
| Placement | Props, splines, seeded scatter rules | Mesh instances |
| Gameplay | Brush entities, point entities, IO connections | Sensor bodies, movers and runtime connections |

The scene document stores authored data only. Generated meshes and
collision are derived data that the scene rebuilds when the data loads or
changes, in the editor and in a packaged game.

### Brush editing

Editing operations change brushes in place, each one journal group: face
moves (`brush.move_face`), vertex moves, clip by a plane, hollow, face
extrude, merge, and carve, which replaces the target with convex pieces once
and keeps no boolean tree. Scene tools add face selection with move handles
and a clip tool. Export to glTF hands a blockout to an artist; Replace with
mesh swaps a brush group for the finished model and keeps its brushes as
collision only.

### Level checks

`level.lint` checks a region against the player character's capsule and limits:
openings narrower than the capsule, ceilings lower than its height, steps
higher than `step_up`, slopes steeper than `max_slope_radians`, unreachable
spawns, overlapping solid brushes and open edges into the void.
`query.reachable(a, b)` answers one path question. The same checks appear in an
editor panel for designers.

### Gameplay volumes

Brush roles (ADR-084) already give solid, visual, clip and trigger volumes;
a trigger brush owns a sensor body. Movers and other gameplay roles come with
IO. Point entities include the existing Player Start, spawns and markers. A
`trigger` component may use a trigger brush or the entity's own box, sphere
or capsule sensor collider.

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

This section is the implementation contract for the phases that remain.
Each phase gains detail when it starts.

### Phase 0: agent channel

Implemented; [ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md)
owns the socket, operations, batches, journal groups, review, captures and
the MCP adapter. Later phases add their operations to the same table.

### Phase 1: brushes

Implemented; [ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md)
owns the components, geometry, rebuild, generated collision, operations and
editor tools. Brush meshes stay per brush: the phase 1 measurement found
about 0.4 µs per visible brush on Bistro, so the cell merge waits until a
level makes that cost visible.

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
| 0. Agent channel | Implemented ([ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md)) | Recorded in ADR-084 |
| 1. Brush core | Implemented ([ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md)) | Recorded in ADR-084 |
| 2. Brush editing | Vertex, edge and face edits, clip, hollow, carve, extrude, brush entities, `level.lint` | Lint tests against named defects (narrow door, high step, steep ramp) |
| 3. IO | `ENTITY` property kind, `io_connection`, the router with the sensor drain, `trigger`, `relay`, `timer`, `counter`, script `VKR_OUTPUTS` and `VKR_INPUTS`, trigger hooks, the IO trace and operations | CPU tests for delivery order, delay deadlines, fire limits, stale targets and the chain limit fault; a Bistro Play run in which a trigger opens a script door |
| 4. Terrain | Terrain-rendering decisions, sculpt and paint, height field collision, region operations | Defined by the terrain-rendering proposal |
| 5. Population | Splines and seeded scatter; linked prefabs stay with ADR-076 | A residency bound for M1 under the 16 GB floor ([ADR-083](../adr/083-supported-hardware-matrix.md)) |

## Risks

- LLM agents make arithmetic and orientation errors in 3D. Intent operations,
  grid snapping, `level.lint`, labelled top-down captures and the review step
  each reduce this; none removes it.

## Sources

- [Chisel features](https://chiselengine.com/features)
- [Valve Hammer Editor](https://developer.valvesoftware.com/wiki/Valve_Hammer_Editor)
- [Source inputs and outputs](https://developer.valvesoftware.com/wiki/Inputs_and_Outputs)
- [TrenchBroom manual](https://trenchbroom.github.io/manual/latest/)
- [Godot CSG tools](https://docs.godotengine.org/en/stable/tutorials/3d/csg_tools.html)
- [Jolt HeightFieldShape](https://jrouwe.github.io/JoltPhysics/class_height_field_shape.html)
- [Model Context Protocol](https://modelcontextprotocol.io/)
- [MCP 2026-07-28 changelog](https://modelcontextprotocol.io/specification/2026-07-28/changelog)
