---
status: proposed
updated: 2026-10-04
authority: proposal
---
# Level design toolkit

The remaining phases of the level design toolkit. The agent channel,
brushes, brush editing, level checks, entity IO and terrain editing are
implemented and recorded in
[ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md); the terrain
material and tile LOD remain in [Terrain rendering](terrain-rendering.md).
This proposal keeps splines and scatter, `terrain.road`, the brush tools no
phase covers yet, and the IO work that follows it. Every phase adds its
operations to the ADR-084 operation table, so the viewport, the Cmd bar and
agents share them.

## Settled decisions

The owner settled these on 2026-10-04. Decisions of the implemented phases
(brushes, the agent channel, MCP 2026-07-28 only, the C adapter, review,
brush storage, brush editing, level checks and entity IO) are recorded in
ADR-084.

| Decision | Choice | Rejected alternatives |
|---|---|---|
| First target | Indoor blockout with brushes; terrain follows | Terrain first; both tracks in parallel |
| Visual scripting | Later work. A future graph asset, in the style of Unreal Blueprints, uses the IO router and its value kinds | Designing the graph editor now |
| Connection targets | An entity in the source's own container, because entity references never cross containers (ADR-076). Named targets across containers can come later | Cross-container references now |

## Current baseline

- **Agent channel, brushes, level checks and IO.** ADR-084: typed
  operations over a local socket and `vkr_mcp`, batches as journal groups
  with review, captures, brush components with generated meshes and
  generated collision bodies, blockout operations, face moves, extrude,
  clip, hollow, carve and merge, face handles and the clip tool,
  `level.lint` and `query.reachable` against the player capsule, and entity
  IO: outputs, inputs and connections from engine components and script
  behaviors, routed after each tick with trigger hooks for scripts.
- **Terrain.** ADR-084: heightfield terrains with sculpt, paint and region
  operations, tile meshes and height field collision. `terrain.road` waits
  for splines.
- **Collision.** Colliders are boxes, spheres, capsules, convex hulls and
  triangle meshes; sensors exist; a body holds at most
  `VKR_PHYSICS_MAX_COLLIDERS` (32) colliders
  ([ADR-072](../adr/072-entity-collision-and-rigid-body-physics.md)); static
  bodies also take height fields.
- **Player metrics.** `VkrPhysicsCharacterDesc` holds the capsule radius and
  half-height, `max_slope_radians`, `step_up` and `step_down`
  ([vkr_physics.h](../../runtime/src/physics/vkr_physics.h)).
- **Prefabs.** `scene.instantiate` copies a project scene under a new root
  without a link to its source.
- **Scripts.** C script modules declare components, fields, behaviors,
  module hooks and IO ports through `sdk.h` macros
  ([ADR-079](../adr/079-c-script-modules.md), [sdk.h](../../sdk/sdk.h)).
- **Descriptor limits.** Properties never describe dynamic arrays
  ([vkr_type_desc.h](../../runtime/src/core/vkr_type_desc.h)). An `ENTITY`
  property references one entity of its own container.
- **Not present.** Brush vertex and edge edits, glTF export of a blockout,
  terrain holes, splines, scatter, movers, and IO across containers. The
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

### Later brush tools

Face moves, extrude, clip, hollow, carve and merge are implemented (ADR-084).
No phase yet covers vertex and edge moves, which must keep a brush convex by
splitting it or refusing the move; export to glTF, which hands a blockout to
an artist; or Replace with mesh, which swaps a brush group for the finished
model and keeps its brushes as collision only.

### After IO

Entity IO is implemented (ADR-084). What follows it:

- **Movers.** A brush role or component that moves between positions on
  inputs, as Source's func_door and func_movelinear do; the sample `door`
  script shows the shape.
- **Targets across containers.** A connection names an entity of its own
  container. Named targets resolved in other loaded containers would let a
  zone scene open a door in the World.
- **Visual scripting.** IO wires entities together, as Source IO and the
  event links of Unreal's Level Blueprint do. A future graph asset, in the
  style of Unreal Blueprints, is a script component whose entry nodes are its
  inputs and whose fire nodes are its outputs. It uses the same router and
  connection data without migration, and its pin kinds are the IO value
  kinds. The behavior proposal's state charts consume the same facts.

### Terrain and population

Terrain rendering finishes in [Terrain rendering](terrain-rendering.md).
Holes for brush-built entrances need the height field's hole samples and a
cut in the tile meshes. `terrain.road(spline)` flattens a band along a
spline once splines exist. Population adds splines
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

Implemented; [ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md)
owns the editing operations, face selection, face handles, the clip tool,
`level.lint`, `query.reachable` and the Level checks window.

### Phase 3: IO

Implemented; [ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md)
owns components, connections, the router, script ports and hooks, the
editor sections and the operations.

### Phase 4: terrain

Editing is implemented;
[ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md#terrain) owns
the heightfield, the scene's terrains, the journal entry, the operations and
the sculpt tool. The terrain material and tile LOD follow
[Terrain rendering](terrain-rendering.md); `terrain.road` ships with phase 5.

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
| 2. Brush editing | Implemented ([ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md)) | Recorded in ADR-084 |
| 3. IO | Implemented ([ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md)) | Recorded in ADR-084 |
| 4. Terrain | Editing implemented ([ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md#terrain)); material and LOD in [Terrain rendering](terrain-rendering.md) | Defined by the terrain-rendering proposal |
| 5. Population | Splines, seeded scatter and `terrain.road`; linked prefabs stay with ADR-076 | A residency bound for M1 under the 16 GB floor ([ADR-083](../adr/083-supported-hardware-matrix.md)) |

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
