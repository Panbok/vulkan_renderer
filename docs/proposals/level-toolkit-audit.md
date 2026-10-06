---
status: proposed
updated: 2026-10-06
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
| A2 | Native Vulkan runs the terrain material, LOD selection, geomorph, streamed terrain, proxies and rebase (H2 to H6, 2026-10-04). Both ADR-085 entries are shared domains in [ADR-044](../adr/044-shader-cross-backend-contract.md): Metal runs them in the tiled pipeline and Vulkan in the desktop pipeline, so their evidence is recorded per backend and no cross-class pixel comparison applies | [Windows record](windows-vulkan-verification.md) | A tiled-pipeline capture of the H4 view on the Mac; the H2 layer blend is checked on the tiled pipeline ([ADR-087](../adr/087-gpu-class-graphics-pipelines.md)) |

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
| A10: each streaming change cost the main thread 1.2–6.4 ms: a 3–4 ms Jolt height field for the body and about 0.6 ms of geometry packing per tile | Bodies build on physics workers and tiles in batches on job workers from copied samples; a body rebuild now costs the main thread under 0.5 ms | `99720bcc` |
| Windows: locals named `far`, `near` and `small` are `windows.h` macros and broke the build; `view.capture` had no captures directory; `[agent]` results were cut at 400 characters; the Cmd queue's 4 KiB could not hold the H7 flight | Renamed; captures go to `vkr\captures` in the user's temporary directory; whole results; a 32 KiB queue | `2e8b4479`, `8d6fed30`, `51eae2ba`, `166857a3` |
| Vulkan rejected geometry publications past about 85 per batch and uploaded one geometry part per staging round trip, so a new terrain took minutes to appear; the editor's Vulkan renderer had no capture ring | Three queued uploads per geometry record; the staging chunk packs every pending buffer upload that fits; the sample runtime enables a one-entry capture ring | `c58696b6`, `9d394791`, `658339e5` |
| A terrain drew its layer weights as a red tint until its layer textures streamed in | The placeholder is a white terrain material | `a905a151` |
| A21: Vulkan refused to grow the geometry megabuffer while any upload was pending; streaming keeps uploads pending, so 2 to 3 terrain tile publications per H7 flight failed | Growth retargets pending uploads, staged ones included, to the new buffers, which the preservation copy precedes; the H7 flight has no failed publication, and a Debug flight that grew the buffer under validation with synchronization checks reports no VUID or hazard | `b4589ea2` |
| A22: an `op` statement only queued its operation, so `scene.save` ran before a queued `brush.box` and captures rendered after the next `view.mode` | `op` holds the Cmd queue until its operation answers (`op cmd` excepted); the H5 script without waits saves both brushes and captures in order | `929cc290` |
| A23: slow frames could not be attributed to render graph passes headless | `VKR_RG_SLOW_FRAME_MS` with `VKR_RG_GPU_TIMING=1` logs each slow frame's three slowest passes; in H7, `Shadow.Local.*` dominates and `Shadow.Cascade.*` rarely appears | `b4dffcae` |
| A20: on Vulkan an 8 km streamed terrain showed thin dark strips along tile-row edges 100 to 250 m from the camera at `[0,60,-700]`. They were cascade shadow cast by holed overview tiles: AMD's driver (26.6.3, RX 6700 XT) offset the pointer cast of `records + lod_record` by the LOD row's 128-byte stride instead of the record's 32, so a 3-level row read zeros and its draws morphed toward a level their indices did not hold. The SPIR-V is correct (`OpPtrAccessChain` at stride 32, then `OpBitcast`) | `vkr_gpu_geometry_lod_row` computes the row's address in bytes; at the H4 view the dark-pixel share falls from 1.03% to 0.18%, the same as with the camera's LOD forced to 0; a Debug run under validation with synchronization checks reports no VUID or hazard; H3 still passes | `42b7a565` |
| A1: moving over a streamed terrain with local shadows on raised the frame p95 from 6.4 ms to about 30 ms; every terrain mesh swap marked every local shadow face and cascade stale, and the cache redrew its 30-face High budget at 0.4–0.9 ms GPU each | Static changes carry the world boxes they may alter and a terrain swap names its changed tiles' footprints, so far streaming redraws no local face; the flight now measures p95 9.0–10.9 ms (indicative). A resident terrain was never affected | `1b4bacea` |

## Verification still needed

| Claim | Status | Step |
|---|---|---|
| CPU behaviour of heightfields, cells, IO refresh and rebase on Windows, and the Windows `file_flush_durable` | Passed 2026-10-04 | H1 |
| Terrain layer blend on Vulkan matches Metal | Runs without validation errors; Metal comparison pending | H2 |
| LOD selection and geomorph on Vulkan, with no validation errors | Passed 2026-10-04; popping unmeasured | H3 |
| Streamed terrain, overview holes and seams on Vulkan | Passed 2026-10-05 | H4 |
| Proxies cook and draw on Windows | Passed 2026-10-04 | H5 |
| Rebase and exact Reset on Vulkan | Passed 2026-10-04 | H6 |
| Hitch measurement on Vulkan (A1) | Measured 2026-10-05 (p95 7.0–9.0 ms, max 9.5–14.7 ms with terrain); slow frames are local shadow redraws | H7 |
| Large-file I/O for an 8 km terrain on Windows (64-bit seeks) | Passed below 2 GiB 2026-10-04 | H8 |
| Agent channel and `vkr_mcp` on Windows | In-process `op` passes; socket and `vkr_mcp` unavailable | H9 |
| Metal side of the one-entry capture ring and the white terrain placeholder | Unrun | Mac |
