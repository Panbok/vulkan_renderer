---
status: proposed
updated: 2026-10-04
authority: proposal
---
# Level toolkit audit

Review of the level design toolkit, terrain, geometry LOD and world partition
([ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md),
[ADR-085](../adr/085-gpu-geometry-lod-and-terrain-geomorphing.md),
[ADR-086](../adr/086-world-partition.md)) at commit `afaac501`. It lists what
is wrong or unproven, with evidence, and what closes each item. Steps named
H1 to H9 are in the
[Windows/Vulkan handoff](level-toolkit-windows-vulkan-handoff.md); future
features are in the [level design toolkit](level-design-toolkit.md) and
[world partition](world-partition.md) proposals.

## Scope and method

- Code: the agent channel and operations, brushes, level checks, entity IO,
  terrain, population, the GPU LOD table and geomorph, terrain streaming,
  cells, the streaming runtime, proxies and the origin rebase.
- Evidence: the macOS Debug CPU suite (all suites pass at `afaac501`),
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
| A1 | Moving the camera over terrain with local shadows on hitches. Gliding at 30 m/s beside Bistro raises the frame p95 from 6.4 ms to about 30 ms (max 32–39 ms) on an M1 Pro, for a resident 1 km terrain and a streamed 8 km one alike. With local shadows off the p95 is about 10 ms. Static views stay near 8 ms. This misses the world partition goal of hitches below one frame | [ADR-086 evidence](../adr/086-world-partition.md#evidence). Freezing the static, publication and caster-bounds generations, keeping released geometry, skipping mesh re-attachment and batching geometry uploads did not help; the render thread waits on the GPU in 74% of samples | A Metal System Trace of the gliding script, then a fix and a matched Release report; H7 repeats the measurement on Vulkan |
| A2 | Native Vulkan has never run the terrain material, LOD selection, geomorph, streamed terrain, proxies or rebase. Both ADR-085 entries in [ADR-044](../adr/044-shader-cross-backend-contract.md) stay UNALIGNED | 105 Vulkan modules pass `spirv-val`; no Vulkan frame was produced | H2 to H6 |

### P2

| # | Finding | Evidence | Closes it |
|---|---|---|---|
| A3 | Saving a streamed terrain writes tiles in place. A crash during the save can leave a file with old and new tiles mixed | `vkr_heightfield_save` writes dirty tiles, then the overview, into the open file | A journal or side file for tiles in flight, and a test that kills a save |
| A4 | A partitioned project scene keeps its cells beside its runtime document path, outside the project store's revisioned overlays | `sample_partition_open` derives `<scene>.cells` from the scene path | The project storage item of the [world partition](world-partition.md) proposal |
| A5 | Script behaviours in cells loaded during Play do not start until the next session; the IO router does refresh | `sample_partition_routed` refreshes routes only | The script lifecycle item of the world partition proposal |
| A6 | While edits are unsaved, no cell unloads, so a long unsaved session over a large world keeps every visited cell | `vkr_scene_edit_cell_unloadable` requires a clean container | Per-cell dirty tracking, so clean cells unload while others hold edits |
| A7 | The editor camera's far plane is 500 m and depth is not reversed, so the overview, distant hills and proxies are clipped unless the far plane is raised by hand | Captures in the 8 km run showed the terrain cut at the far plane until `view.camera` set `far` | The long views item of the world partition proposal |
| A8 | Origin rebase moves positions under scripts that keep world positions | No script notification exists | An origin event in the script SDK |

### P3

| # | Finding | Note |
|---|---|---|
| A9 | An 8 km terrain at 1 m is a 409 MiB file: 2-byte heights and 4-byte weights per sample, tiles padded to 65 samples | Compression or coarser weights would cut it; the samples in memory stay windowed |
| A10 | Each streaming change rebuilds the whole terrain mesh attachment (about 300 submeshes, 1.7 ms) and, when the window moves, one 324-by-324 height field (3.5 ms) | Measured in the streamed drive; budget them after A1 |
| A11 | One collision body covers the sources' window; with sources more than 16 tiles apart, only the first source has collision | Physics requires unique body and collider entity ids |
| A12 | A cell document holds at most 1,024 objects, and saving looks up overlay ids linearly | `EDIT_CREATED_MAX`, `edit_created_id` |
| A13 | Proxies cover solid and visual brushes only, with world-projected UVs on the brushes' materials. A cell whose bake left no proxy stays without one until the scene reopens | `vkr_proxy_cell`, `VKR_SCENE_PARTITION_CELL_NO_PROXY` |
| A14 | Imported documents (Bistro's content), the root World and added scenes do not partition | ADR-086 alternatives |
| A15 | Proxy and other runtime-only entities appear in `scene.describe` regions and can be picked | They carry the transient tag, which saving skips |
| A16 | The editor grid's axis lines stay at the world origin while the origin is rebased | Cosmetic; the grid is not document-anchored |

## Fixed during the audit

| Item | Fix | Commit |
|---|---|---|
| Saving or editing during a paused, rebased Play would write positions offset by the rebase into documents | Edits, batches and saves are refused while any container's origin is rebased | `afaac501` |
| Starting Play cleared the pins the designer set with `partition.load` | Play now holds cells with its own flag; user pins survive Play | `afaac501` |
| The cell index had no record of overlay ids, so a new object could take the id of an object in an unloaded cell | `index.json` keeps `next_id`; a load replaces only ids a loaded object holds | `3d13bdaa` |
| Streaming tried a rejected terrain body every frame | Rejected bodies wait until the samples change | `37ff46f2` |

## Verification still needed

| Claim | Status | Step |
|---|---|---|
| CPU behaviour of heightfields, cells, IO refresh and rebase on Windows | Unrun | H1 |
| Terrain layer blend on Vulkan matches Metal | Unrun | H2 |
| LOD selection and geomorph on Vulkan, with no validation errors | Unrun | H3 |
| Streamed terrain, overview holes and seams on Vulkan | Unrun | H4 |
| Proxies cook and draw on Windows | Unrun | H5 |
| Rebase and exact Reset on Vulkan | Unrun | H6 |
| Hitch measurement on Vulkan (A1) | Unrun | H7 |
| Large-file I/O for an 8 km terrain on Windows (64-bit seeks) | Unrun | H8 |
| Agent channel and `vkr_mcp` on Windows | Unrun | H9 |
