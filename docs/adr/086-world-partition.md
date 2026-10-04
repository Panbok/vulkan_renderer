---
status: partial
updated: 2026-10-04
authority: adr
---

# ADR-086: World partition

## Status

Accepted (partial). Terrain streaming, cells with cell documents, the
streaming runtime, origin rebasing during Play, baked cell proxies, the World
Partition window and the `partition.*` operations are implemented and
verified on Metal. Streaming hitches stay above one frame while the camera
moves over a streamed terrain with local shadows on, imported scene
documents do not split into cells, and native Vulkan execution is
unverified. The
[level toolkit audit](../proposals/level-toolkit-audit.md) tracks these.

## Context

A terrain kept every sample resident up to 1,024 cells
([ADR-084](084-agent-channel-and-level-design-toolkit.md#terrain)), and every
entity of a loaded container stayed loaded. The owner accepted the
[World partition](../proposals/world-partition.md) proposal on 2026-10-04
with its recommendations: cells stream entities in and out of their scene's
one container, 128 m cells, per-cell proxies built by `vkr_bakery`, an 8 km
by 8 km target, and origin rebasing past 4 km. After measuring the precision
at 8 km (0.5 mm within 8 km of the origin), the owner kept the full rebase
during Play.

## Decision

### Streaming sources

The host sets each loaded container's streaming sources every frame
(`vkr_scene_set_stream_sources`,
[vkr_scene_system.h](../../runtime/src/renderer/systems/vkr_scene_system.h)):
the camera that draws the viewport, the editor's or a game's. Terrains, cells
and proxies stream around them; a scene without sources streams nothing in.

### Terrain streaming

A heightfield ([vkr_heightfield.h](../../runtime/src/level/vkr_heightfield.h))
holds 64-sample tiles, each resident or not. Terrains up to 1,024 cells keep
every tile, as before. Larger ones, up to 8,192 cells and a multiple of
1,024, stream:

- **File.** Version 2 of `VKRHFLD1` appends an overview of every 16th sample
  after the tiles; version 1 files still load. `vkr_heightfield_create_file`
  writes a new terrain tile by tile without holding it. A streamed terrain
  saves its changed tiles and overview to `<file>.journal` first, stored on
  the disk with a checksum, then writes them in place and removes the
  journal; opening the file finishes a whole journal and drops a torn one.
  A file that cannot be written streams read-only and refuses saves.
- **Tiles.** Fine tiles within the terrain's `stream_radius` (512 m by
  default) of a source build, the 64 nearest considered per update, within
  2 ms; they stay until a tile past the radius. A fine tile loads its sample
  tiles and their neighbours first. Samples an edit or undo reaches load
  synchronously and stay resident until a save writes them. Rebuilt tiles
  show when their geometry has uploaded: the mesh re-attaches then, and the
  tiles that draw hold still meanwhile.
- **Overview.** 64-cell overview tiles at 16 times the spacing draw the rest.
  An overview tile leaves out the cells under fine tiles that draw, keeps only
  the three levels whose cells lie inside one fine tile, and adds seam skirts
  facing into the holes. Streamed skirts reach 32 spacings down.
- **Collision.** Physics refuses two bodies or two colliders with one entity
  id, so a streamed terrain has one height field body over the 5 by 5 tiles
  around the sources' tiles (`VKR_SCENE_TERRAIN_BODY_TILES`), or around the
  first source when the sources span more than 16 tiles. It rebuilds when the
  window moves and after edits rest.

`scene.describe` reports a terrain's tiles in memory, drawing and unsaved.

### Cells

A scene with a `world_partition` singleton
([vkr_scene_types.c](../../runtime/src/renderer/systems/vkr_scene_types.c))
divides the ground plane into square cells (`cell_size`, 128 m by default).
An editor-created root entity streams with the cell holding its world
position, and its children with it
([vkr_scene_partition.c](../../runtime/src/renderer/systems/vkr_scene_partition.c)).
Document entities, terrains, singletons, runtime-only entities and entities
with an enabled `always_loaded` component form the persistent layer. A cell
is chosen by position rather than bounds, so it is known before meshes load.

### Cell documents

A partitioned scene `<scene>.json` keeps its cells in `<scene>.cells/`
([vkr_scene_edit.c](../../runtime/src/renderer/systems/vkr_scene_edit.c)):

- `<x>_<z>.json` holds `{"version":1,"cell":[x,z],"created":[...]}` with the
  overlay's created-record schema. The overlay keeps the persistent layer.
- `index.json` lists the documents, their cell size and the next overlay id,
  so new objects never take an id an unloaded cell uses. A loaded object
  holding a file's id gets a fresh one.
- Saving merges a cell's document before an object moved into it is written,
  writes each loaded cell whose bytes changed, removes the documents of loaded
  cells left empty and keeps the documents of unloaded cells. A cell size
  change loads every cell and replaces the old documents at the next save.
  Cell documents, the index and the overlay are all written beside their
  files and stored on the disk before any replaces its file, so a save that
  fails while writing changes none of them.
- A document that cannot be read marks its cell unreadable: the cell does
  not stream, the index keeps listing it, and a save that would put an
  object in it is refused. A listed document that is missing loads as an
  empty cell.
- A cell loading while the origin is rebased places its roots offset by the
  rebase.

### Streaming runtime

Each frame the runtime plans loads and unloads for the primary scene
([vkr_sample_runtime.c](../../runtime/src/vkr_sample_runtime.c)):

- Cells with a document within `load_radius` (384 m) load nearest first, one
  always and more within 2 ms. Cells past the radius plus one cell unload, and
  past `cell_budget` (256 loaded cells) the farthest unload first.
- A cell unloads only when no journal entry names one of its objects and
  either the container has no unsaved edit or the cell, written as a save
  would write it, matches its document byte for byte
  (`vkr_scene_edit_cell_unloadable`). Undo always finds its objects, and
  unsaved edits elsewhere no longer keep every visited cell loaded. The
  comparison is remembered until the next edit.
- `partition.load` and the World Partition window pin cells for editing;
  pinned cells never unload until released.
- Play holds the cells it starts with only while the scene has unsaved edits.
  Reset unloads cells loaded during Play and reloads the starting cells that
  left. While the origin is rebased, edits and saves are refused, because the
  world holds positions the documents do not.
- A session's IO router refreshes after cells come or go
  (`vkr_io_router_refresh`,
  [vkr_io_router.h](../../runtime/src/script/vkr_io_router.h)), keeping the
  state of components that stayed.

### Proxies

`vkr_bakery bake proxies --scene <scene.json>`
([vkr_bakery_bake.c](../../tools/bakery/vkr_bakery_bake.c)) rebuilds each
cell document's solid and visual brushes, welds them by position and
material, simplifies them with meshoptimizer within 0.25 m, writes
`<scene>.cells/proxies/<x>_<z>/proxy.gltf` with world-projected UVs, cooks it
with `tool mesh`, and binds each range to its brushes' material. An unchanged
source is not cooked again. The runtime draws the proxy of each unloaded cell
with a document within `proxy_radius` (2,048 m) on a runtime-only entity.

### Origin rebase

During Play, a camera more than 4,096 m from the origin along X or Z moves
every container back by whole kilometres:

- `vkr_scene_shift_origin` moves root entities and records their positions
  before the first shift; `origin_offset` is document minus world position.
- `vkr_scene_physics_shift` moves every Jolt body and character once per
  world, and each scene's interpolation poses, authored world matrices, root
  positions, character states and generated collider copies.
- Cameras move with the world. Partition math, proxy placement and the window
  map work in document space. The cloud wind offset, reflection probe centres,
  the diffuse volume origin and fog density boxes add the offset, so they stay
  where the document puts them. The 1 km step exceeds the temporal camera-cut
  distance, so temporal history resets on that frame.
- Reset restores the recorded root positions exactly, then moves physics
  back, before physics resets.

### Tools

The World Partition window maps 15 by 15 cells around the camera: pinned,
loaded, saved and proxy cells. A click loads and pins a cell or releases a
pinned one. Cmd `partition.load` and `partition.unload`, and the operations
`partition.describe`, `partition.load` and `partition.unload`
([editor_ops.c](../../editor/src/editor_ops.c)), give agents the same view.
`view.camera` takes `far` for the far plane and `glide` for moves without a
lens change or temporal cut; `stats.frame_ms_max` reports the longest recent
frame.

## Consequences

- An 8 km terrain is a 409 MiB file at 1 m spacing. With Bistro beside it,
  the editor's resident set stayed between 745 and 780 MB on an M1 Pro.
- Streaming one change costs about 2.4 ms for tile builds, 1.7 ms to rebuild
  the mesh and re-attach every tile, and 3.5 ms when the collision window
  moves. The measured hitches come from local shadows: see Evidence.
- Saving rewrites only edited cells, so version control diffs stay local.
- Scripts that keep world positions see them jump when the origin rebases.
- Only the primary scene partitions; the World and added scenes keep every
  entity.
- Behaviours of script components in a cell loaded during Play start at the
  next frame, as for any entity that gains the component; an unloading cell
  runs their `destroy` and `stop`. Their state does not survive the unload.
- Proxies cover brushes only. A cell whose bake left no proxy looks again
  when the proxies directory changes.
- A scatter over a streamed terrain lands on its samples where no physics
  body reaches, so its copies do not depend on where the camera was.
- The editor camera's far plane is 500 m by default and depth is not reversed,
  so the overview and proxies show only with a far plane raised by hand.

## Evidence

Indicative headless Release runs on an M1 Pro (Metal), Bistro, not matched
harness reports:

| Run | Frame mean / p95 / max (ms) |
| --- | --- |
| Bistro, gliding 30 m/s, high preset | 6.1 / 6.4 / 7.3–8.9 |
| Bistro beside an 8 km streamed terrain, same flight | 3.1–8.6 / 29.8–32.6 / 29.7–39.4 |
| Same, low preset | 7.7–8.9 / 8.0–11.7 / 15.7–32.0 |
| Same, high preset with local shadows off | 8.7–9.4 / 9.8–21.2 / 17.8–43.7 |

The hitches are local shadow redraws. Every completed resource publication,
a streamed tile's geometry upload included, advances the renderer's global
publication generation, which marks every local shadow face stale; the
cache then redraws its face budget (30 faces on High) at 0.4–0.9 ms GPU each
(`VKR_RG_GPU_TIMING=1` pass rows, `Shadow.Local.*`). A diagnostic build that
ignored the publication generation in that test kept the same flight at
p95 10.5 ms. Re-attaching the mesh before its tiles uploaded also left it
unpublished for a frame, which removed the terrain from that frame and made
two more frames redraw; the mesh now re-attaches after the upload. At
`f01956ee` a resident 1 km terrain on the same flight measured p95 8.7 ms,
so the earlier report that it hitched too did not reproduce.

Retained shadows now follow only publications that can reach a drawn caster
([ADR-019](019-bounded-forward-spatial-lighting.md)), and adding or removing a
drawn mesh is a topology change. Each re-attached terrain mesh therefore
moves the static-world generation, which still marks every local face and
cascade stale: the same flight measured p95 14.5–29.9 ms (max 30.2–48.1 ms)
over five readings. Shadow staleness bounded by where the static change
happened remains.

CPU tests: the heightfield suite (a 2,048-cell streamed field that loads only
its overview, edits, saves in place and reloads a tile), the scene edit
partition test (cell documents, byte-identical unedited cells, the unload
rules), the IO router refresh test, and the physics rebase test (a body keeps
falling onto a shifted floor; restoring returns exact bits). Headless Bistro
runs verified tile streaming and collision under the camera, cells loading
and unloading by distance, pinning, proxy drawing for an unloaded cell, and a
rebase to −4,096 m that Reset returned to 0.

## Alternatives considered

- **A container per cell.** Rejected by the owner: it hits the seven-container
  limit, splits ECS worlds and breaks references across cells.
- **A height field body per tile.** Rejected: physics requires unique body and
  collider entity ids, and synthetic ids would leak into queries.
- **Partitioning imported documents.** Deferred: it needs the scene document
  format to split, not only the overlay.
- **Camera-relative rendering instead of a rebase.** The owner chose the full
  rebase.

## Revisit when

- Static shadow changes carry the bounds of what changed, or a matched
  Release report shows streaming hitches below one frame.
- A Windows/Vulkan run of the
  [handoff](../proposals/level-toolkit-windows-vulkan-handoff.md) passes.
- Imported scenes, the World or added scenes need to stream.
