---
status: proposed
updated: 2026-10-04
authority: proposal
---
# Level design toolkit

Work that follows the level design toolkit. Its six phases are implemented
and recorded in [ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md),
with terrain levels in
[ADR-085](../adr/085-gpu-geometry-lod-and-terrain-geomorphing.md): the agent
channel, brushes, brush editing and level checks, entity IO, terrain, and
population. This proposal keeps the brush tools, gameplay pieces, terrain
holes and population features no phase covered, and the visual scripting
that builds on IO. Each addition puts its operations in the ADR-084
operation table, so the viewport, the Cmd bar and agents share them.

## Settled decisions

The owner settled these on 2026-10-04. Decisions of the implemented phases
are recorded in ADR-084 and ADR-085.

| Decision | Choice | Rejected alternatives |
|---|---|---|
| Visual scripting | Later work. A future graph asset, in the style of Unreal Blueprints, uses the IO router and its value kinds | Designing the graph editor now |
| Connection targets | An entity in the source's own container, because entity references never cross containers (ADR-076). Named targets across containers can come later | Cross-container references now |
| Large worlds | Terrain and scene content stream by cells in a separate proposal, [World partition](world-partition.md) | Streaming inside the toolkit |

## Current baseline

- **Toolkit.** ADR-084: typed operations over a local socket and `vkr_mcp`,
  batches as journal groups with review, captures, brushes with generated
  meshes and collision, brush editing, `level.lint` and `query.reachable`,
  entity IO, heightfield terrain with a layered material and height field
  collision, splines, spline meshes, scatter and `terrain.road`.
- **Prefabs.** `scene.instantiate` copies a project scene under a new root
  without a link to its source.
- **Descriptor limits.** Properties never describe dynamic arrays
  ([vkr_type_desc.h](../../runtime/src/core/vkr_type_desc.h)), so lists such
  as spline points are child entities.
- **Not present.** Brush vertex and edge edits, glTF export of a blockout,
  Replace with mesh, movers, IO across containers, terrain holes, meshes
  bent along a spline and linked prefabs. The
  [behavior proposal](entity-behavior-system.md#second-deliverable-connections-and-constrained-state-charts)
  plans connection assets that bind a typed event to an action on an entity.

## Lessons from other engines

| Engine | Take | Leave |
|---|---|---|
| Source (Hammer) | Brush entities such as movers, inputs and outputs | BSP, VIS, leak checks and lightmap compiles |
| Unreal Engine 5 | Spline meshes that bend along the curve, PCG graphs, Blueprints | Legacy BSP |
| Unity | Face inset and bevel (ProBuilder) as brush operations | Editable meshes as the source of architecture |
| Godot | Tile-kit snapping for modular props (GridMap) | CSG nodes |

## Proposed work

### Brush tools

- **Vertex and edge moves** that keep a brush convex, by splitting it or
  refusing the move.
- **Export to glTF**, which hands a blockout to an artist.
- **Replace with mesh**, which swaps a brush group for the finished model and
  keeps its brushes as collision only.

### Gameplay

- **Movers.** A brush role or component that moves between positions on
  inputs, as Source's func_door and func_movelinear do; the sample `door`
  script shows the shape.
- **Targets across containers.** Named targets resolved in other loaded
  containers would let a zone scene open a door in the World.
- **Visual scripting.** A future graph asset is a script component whose
  entry nodes are its inputs and whose fire nodes are its outputs. It uses
  the IO router and connection data without migration, and its pin kinds
  are the IO value kinds.

### Terrain and population

- **Holes** for brush-built entrances: the height field's hole samples
  (`VKR_PHYSICS_HEIGHT_HOLE`) and a cut in the tile meshes and levels.
- **Bent spline meshes**, which deform each copy's vertices along the curve
  for kerbs, fences and pipes, in addition to the rigid copies.
- **Linked prefabs**, once the behavior proposal defines their lifecycle
  ([ADR-076](../adr/076-project-object-model.md)).
- **Scatter rules** that a designer can stack, as PCG does: density from
  terrain layers, slope limits and exclusion near roads.

## Acceptance evidence

Each item lands with the CPU test that falsifies its contract and a
headless Bistro run that exercises its operations and captures the result.

## Risks

- LLM agents make arithmetic and orientation errors in 3D. Intent operations,
  grid snapping, `level.lint`, labelled top-down captures and the review step
  each reduce this; none removes it.

## Sources

- [Valve Hammer Editor](https://developer.valvesoftware.com/wiki/Valve_Hammer_Editor)
- [Source inputs and outputs](https://developer.valvesoftware.com/wiki/Inputs_and_Outputs)
- [Unreal spline mesh components](https://dev.epicgames.com/documentation/en-us/unreal-engine/blueprint-spline-mesh-components-in-unreal-engine)
- [Unreal procedural content generation](https://dev.epicgames.com/documentation/en-us/unreal-engine/procedural-content-generation-overview)
