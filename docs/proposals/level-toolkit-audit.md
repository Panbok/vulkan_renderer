---
status: proposed
updated: 2026-10-04
authority: proposal
---
# Level toolkit audit

Review of the level design toolkit, terrain, geometry LOD and world partition
([ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md),
[ADR-085](../adr/085-gpu-geometry-lod-and-terrain-geomorphing.md),
[ADR-086](../adr/086-world-partition.md)) at commit `afaac501`, with a
second code review at `b6c5fa95` whose fixes landed in `e9a17eb3` and
`f01956ee`. It lists what is wrong or unproven, with evidence, and what
closes each item. Steps named
H1 to H9 are in the
[Windows/Vulkan handoff](level-toolkit-windows-vulkan-handoff.md); future
features are in the [level design toolkit](level-design-toolkit.md) and
[world partition](world-partition.md) proposals.

## Scope and method

- Code: the agent channel and operations, brushes, level checks, entity IO,
  terrain, population, the GPU LOD table and geomorph, terrain streaming,
  cells, the streaming runtime, proxies and the origin rebase.
- Evidence: the macOS Debug CPU suite (all suites pass at `afaac501` and,
  with address and undefined-behaviour sanitizers, at `e9a17eb3`),
  headless Release editor runs on Bistro through the agent socket, Metal API
  validation of the terrain and LOD paths, `spirv-val` of the Vulkan modules,
  and indicative timings with `stats.frame_ms_max` and gliding
  `view.camera` moves.
- Not covered: native Vulkan execution, Windows file and path behaviour,
  matched harness reports and long editing sessions by people.

## Severity

| Level | Meaning |
|---|---|
| P1 | Misses an accepted goal or budget, or can lose or corrupt data |
| P2 | Wrong or fragile behaviour in a supported path |
| P3 | Limit, cost or tooling gap to record |

## Findings

### P1

| # | Finding | Evidence | Closes it |
|---|---|---|---|
| A2 | Native Vulkan has never run the terrain material, LOD selection, geomorph, streamed terrain, proxies or rebase. Both ADR-085 entries in [ADR-044](../adr/044-shader-cross-backend-contract.md) stay UNALIGNED | 105 Vulkan modules pass `spirv-val`; no Vulkan frame was produced | H2 to H6 |

### P2

| # | Finding | Evidence | Closes it |
|---|---|---|---|
| A4 | A partitioned project scene keeps its cells beside its runtime document path, outside the project store's revisioned overlays | `sample_partition_open` derives `<scene>.cells` from the scene path | The project storage item of the [world partition](world-partition.md) proposal |
| A7 | The editor camera's far plane is 500 m and depth is not reversed, so the overview, distant hills and proxies are clipped unless the far plane is raised by hand | Captures in the 8 km run showed the terrain cut at the far plane until `view.camera` set `far` | The long views item of the world partition proposal |
| A8 | Origin rebase moves positions under scripts that keep world positions | No script notification exists | An origin event in the script SDK |

### P3

| # | Finding | Note |
|---|---|---|
| A9 | An 8 km terrain at 1 m is a 409 MiB file: 2-byte heights and 4-byte weights per sample, tiles padded to 65 samples | Compression or coarser weights would cut it; the samples in memory stay windowed |
| A10 | Moving the terrain body rebuilds one 324-by-324 Jolt height field in a single update (3–4 ms), now at every second tile crossing; tile and overview rebuilds stay within the 2 ms streaming budget | Measured in the streamed drive; building the shape off the main thread would remove it |
| A11 | One collision body covers the sources' window; with sources more than 16 tiles apart, only the first source has collision | Physics requires unique body and collider entity ids |
| A12 | A cell document holds at most 1,024 objects, and saving looks up overlay ids linearly | `EDIT_CREATED_MAX`, `edit_created_id` |
| A13 | Proxies cover solid and visual brushes only, with world-projected UVs on the brushes' materials | `vkr_proxy_cell` |
| A14 | Imported documents (Bistro's content), the root World and added scenes do not partition | ADR-086 alternatives |
| A15 | Proxy and other runtime-only entities appear in `scene.describe` regions and can be picked | They carry the transient tag, which saving skips |
| A16 | The editor grid's axis lines stay at the world origin while the origin is rebased | Cosmetic; the grid is not document-anchored |
| A17 | A brush stroke's undo entry keeps the samples of the rectangle around all its steps; past 512 by 512 samples the stroke continues in another entry of its group, so a stroke across a whole 8 km terrain could still need several hundred entries | `EDIT_TERRAIN_FOLD_SAMPLES` |
| A18 | Scatter placement on a streamed terrain reads bilinear samples where no physics body reaches, while the body is triangulated; copies differ by the sample interpolation on steep slopes | `vkr_scene_terrain_ground` |
| A19 | The terrain streamer scans every fine tile each update to release those past the window (16,384 slots at 8 km) | `terrain_stream` |

## Fixed during the audit

| Item | Fix | Commit |
|---|---|---|
| Saving or editing during a paused, rebased Play would write positions offset by the rebase into documents | Edits, batches and saves are refused while any container's origin is rebased | `afaac501` |
| Starting Play cleared the pins the designer set with `partition.load` | Play now holds cells with its own flag; user pins survive Play | `afaac501` |
| The cell index had no record of overlay ids, so a new object could take the id of an object in an unloaded cell | `index.json` keeps `next_id`; a load replaces only ids a loaded object holds | `3d13bdaa` |
| Streaming tried a rejected terrain body every frame | Rejected bodies wait until the samples change | `37ff46f2` |
| A cell loading during Play past a rebase placed its objects at their document positions, 4 km or more from where they belonged | Loads offset roots by the rebase | `e9a17eb3` |
| A failed cell load cleared the document flag, so the next save dropped the cell from the index and an object moved into it overwrote the document | Unreadable cells stay listed and unloaded, saves putting objects in them are refused, and a listed but missing document loads as an empty cell | `e9a17eb3` |
| The overlay was written before the cell documents, so a failed cell write lost an object moving between them; cell documents were not stored on the disk before their rename | Every document and the overlay are written and stored beside their files before any replaces its file (`file_flush_durable`) | `e9a17eb3` |
| Load and unload plans past 64 candidates kept an arbitrary set instead of the nearest or farthest | Full lists replace their worst entry | `e9a17eb3` |
| A3: saving a streamed terrain wrote tiles in place, so a crash could mix old and new tiles | A checksummed journal precedes the in-place writes; opening finishes a whole journal and drops a torn one | `e9a17eb3` |
| A5: reported that behaviours in cells loaded during Play do not start; the script host starts them at the next frame like any entity gaining the component | No defect; the proposal item now covers keeping their state across unloads | — |
| A6: no cell unloaded while any edit was unsaved | A cell unloads when it matches its document byte for byte and the journal names none of its objects | `e9a17eb3` |
| A13 (part): a cell whose bake left no proxy stayed without one until the scene reopened | It looks again when the proxies directory changes | `e9a17eb3` |
| A streamed terrain opened read-only, as in a packaged game, failed to load | It streams read-only and refuses saves | `e9a17eb3` |
| A scatter over a streamed terrain placed copies only inside the body window around the camera at build time | Columns the ray misses or meets below the terrain take the terrain's samples | `e9a17eb3` |
| The flatten brush overshot its target with a strength above 1 | Strength clamps to 0–1 as for smooth and paint | `e9a17eb3` |
| Tile streaming kept the first 2,048 candidates in scan order, breaking nearest-first loading for detail radii past about 1.4 km, and counted tiles near two sources twice | Each tile counts once and the 64 nearest are kept | `e9a17eb3` |
| Restoring the origin compared every root with every saved root | One ordered pass | `e9a17eb3` |
| A long brush stroke reallocated and copied its ever larger undo rectangle at every step | The stroke continues in a grouped entry past 512 by 512 samples | `e9a17eb3` |
| A re-attached terrain mesh waited unpublished for its new tiles' uploads, so the terrain left that frame and every local shadow redrew twice more | The mesh re-attaches once its tiles have uploaded; streaming holds the drawn tiles until then | `f01956ee` |
| Every completed publication, geometry uploads included, marked every retained shadow (local faces and cascades) stale; adding or removing a generated mesh did not move the static-world generation, so retained shadows and Metal's static candidate rows relied on that publication churn | Retained shadows follow only texture, sampler and material publications, and adding or removing a drawn mesh is a topology change | `b2de2834` |
| A1: moving over a streamed terrain with local shadows on raised the frame p95 from 6.4 ms to about 30 ms; every terrain mesh swap marked every local shadow face and cascade stale, and the cache redrew its 30-face High budget at 0.4–0.9 ms GPU each | Static changes carry the world boxes they may alter and a terrain swap names its changed tiles' footprints, so far streaming redraws no local face; the flight now measures p95 9.0–10.9 ms (indicative). A resident terrain was never affected | `1b4bacea` |

## Verification still needed

| Claim | Status | Step |
|---|---|---|
| CPU behaviour of heightfields, cells, IO refresh and rebase on Windows, and the Windows `file_flush_durable` | Unrun | H1 |
| Terrain layer blend on Vulkan matches Metal | Unrun | H2 |
| LOD selection and geomorph on Vulkan, with no validation errors | Unrun | H3 |
| Streamed terrain, overview holes and seams on Vulkan | Unrun | H4 |
| Proxies cook and draw on Windows | Unrun | H5 |
| Rebase and exact Reset on Vulkan | Unrun | H6 |
| Hitch measurement on Vulkan (A1) | Unrun | H7 |
| Large-file I/O for an 8 km terrain on Windows (64-bit seeks) | Unrun | H8 |
| Agent channel and `vkr_mcp` on Windows | Unrun | H9 |
