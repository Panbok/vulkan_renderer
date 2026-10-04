---
status: proposed
updated: 2026-10-04
authority: proposal
---
# World partition

Worlds larger than what fits in memory at once, streamed by cells around the
camera or the player. The owner chose a full partition on 2026-10-04:
terrain and scene content both stream, not terrain alone. This proposal
starts after the level design toolkit
([ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md)), whose
terrain file was laid out for it.

## Current baseline

- **Containers.** A project has the root World, a primary scene and up to six
  additive scenes, each a `VkrScene` with its own ECS world, document,
  overlay and journal ([ADR-076](../adr/076-project-object-model.md)).
  Containers load asynchronously and stay loaded until removed; every entity
  of a loaded container is resident. Entity references never cross
  containers. Render ids are partitioned per container, and all containers
  share one Jolt world.
- **Terrain.** A terrain keeps every sample resident, up to 1,025 samples a
  side at 1 m spacing (6 MiB of samples and 256 tile geometries)
  ([ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md#terrain)).
  The `VKRHFLD1` file stores 64-sample tiles that can be read one by one.
  Tiles have seven geomorphing levels selected on the GPU
  ([ADR-085](../adr/085-gpu-geometry-lod-and-terrain-geomorphing.md)) and one
  height field collider covers the whole terrain.
- **Residency.** Material textures stream under a byte budget, meshes load
  through the resource system, the mesh manager holds 16,384 instances, and
  population is bounded at 4,096 copies per scene.
- **Floor.** Production Bistro must fit a 16 GB Mac and an 8 GB GPU
  ([ADR-083](../adr/083-supported-hardware-matrix.md)); the M1 is memory bound.
- **Not present.** Streaming of entities, terrain tiles or collision by
  distance; proxies for unloaded content; a world larger than one terrain.

## Goal

An 8 km by 8 km world, terrain and placed content, that an M1 with 16 GB
edits and plays within Bistro's frame budget, with streaming hitches below
one frame at walking and driving speeds. Agents and designers address it by
cells and regions, as they address one scene today.

## Proposed design

### Grid and cells

A partitioned scene divides the ground plane into square cells of a fixed
size, a multiple of the 64 m terrain tile (128 m by default). Each entity
belongs to the cell that holds its bounds' centre; an entity flagged
always-loaded, and every World-only and singleton object, belongs to the
scene's persistent layer instead. Children belong to their root's cell, so a
brush, its faces and its connections move together. Streaming sources (the
editor camera, each player in a game) load the cells within a radius per
layer: a smaller radius for small props, a larger one for terrain and
landmarks.

### Storage

The partitioned scene's document holds the persistent layer and the grid;
each cell is a document of its own beside it, so saving writes only changed
cells and version control diffs stay local. Generated data (brush meshes,
tile meshes, population copies) stays derived, as now.

### Terrain streaming

A world terrain spans many cells. Its file grows to a grid of `VKRHFLD1`
regions, and the scene keeps a window of 64-sample tiles resident around each
streaming source, reading tiles as cells load. Beyond the window, a coarse
overview of the whole terrain (every 16th sample) stays resident and renders
the horizon. Collision becomes one height field per loaded region, so
physics follows the same window. Edits keep working on resident tiles; the
journal holds their samples as today.

### Distant content

Unloaded cells show proxies: merged, simplified meshes that `vkr_bakery`
builds from each cell's static content, in the style of UE5's HLOD. Proxy
simplification shares the cooker of the automatic mesh LOD proposal. Lights,
gameplay entities and population copies have no proxy.

### Streaming runtime

Cell loads run through the resource system off the frame. Activation (entity
creation, mesh instances, bodies) runs under a per-frame time budget so a
burst of cells spreads over frames. A memory budget derived from the
hardware floor bounds the loaded cells; the farthest cells unload first.
Population rules run when their cell loads.

### References, IO and scripts

References inside one cell or to the persistent layer always resolve.
References to another cell resolve while it is loaded; IO deliveries to an
unloaded target are dropped and counted, as deliveries to destroyed targets
are today ([ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md#entity-io)).

### Editor and agents

A World Partition window shows the grid as a map with loaded, dirty and
proxy states, and loads a dragged region for editing. Operations
`partition.describe`, `partition.load` and `partition.unload` give agents
the same view; describe and lint operations take regions as today and report
cells that are not loaded.

## Open decisions

| Decision | Recommendation | Alternative and cost |
|---|---|---|
| A cell's runtime form | Cells stream entities in and out of their scene's one container, so references and the journal keep their meaning | A container per cell: simple unloading, but the seven-container limit, per-container ECS worlds and references across cells break |
| Default cell size | 128 m (two terrain tiles) | 256 m halves cell counts but loads coarser |
| Proxies | Built by `vkr_bakery` per cell, with the mesh LOD simplifier | Impostor billboards: cheaper to build, poor at grazing angles |
| Target world size | 8 km by 8 km | 4 km first, deferring 64-bit-safe origins |
| Large coordinates | Rebase the origin per streaming source past 4 km | Keep 32-bit world positions and accept precision loss far out |

## Phases

| Phase | Scope | Acceptance evidence |
|---|---|---|
| 1. Baseline | Measure Bistro plus a 4 km terrain: memory, frame time and load time of the current all-resident model | A matched Release report on M1 Pro |
| 2. Terrain streaming | Region files, a tile window per source, the coarse overview, windowed collision | An 8 km terrain fits the floor; walking crosses regions without a hitch above one frame |
| 3. Cells | Cell assignment, cell documents, the persistent layer, editor conversion of a scene | Saving changes only edited cells; a converted Bistro renders as before |
| 4. Streaming runtime | Budgeted cell loads and activation, unloading by distance | Driving across a populated 4 km world stays within budget |
| 5. Proxies | Per-cell proxies from the bakery | The horizon shows unloaded cells; proxy memory per cell is bounded |
| 6. Tools | The World Partition window and `partition.*` operations | Headless agent runs load, edit and save a region |

## Evidence needed

- Matched capture-free Release reports on M1 Pro for each phase, with
  memory high water, frame mean and p95, and streaming hitch percentiles,
  using Bistro with a large terrain beside it.
- A matched Vulkan run on the Windows reference machine.
- Picking, physics queries and IO checked across cell boundaries while cells
  load and unload.

## Risks

- Streaming moves cost from load time to every frame; budgets that are too
  tight leave holes near fast sources.
- Cell assignment by bounds can split content a designer sees as one piece;
  children follow their root to limit this.
