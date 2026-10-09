---
status: partial
updated: 2026-10-09
authority: adr
---

# ADR-096: Custom material graphs

## Status

Accepted (partial). A material graph that the Standard tier cannot lower
becomes a Custom graph. It gets a generated surface function, its own
pipelines on the Metal tiled pipeline and a Standard fallback while those
pipelines are being created. Vulkan draws every Custom material's fallback
until a Windows host can verify the desktop resolve (owner, 2026-10-09).

## Context

[ADR-093](093-material-graphs-and-art-workbench.md) lowers graphs to
material rows and costs no shader. Graphs that need per-pixel math, such as
time, world coordinates or arbitrary arithmetic, did not load.
[The artist toolkit proposal](../proposals/artist-toolkit.md) (Parts 3 and
4) lets such a graph cost a shader once, under a budget, never multiplied by
instances.

The owner settled three points on 2026-10-09:
- Metal draws Custom graphs through per-graph buckets of the GPU-driven draw
  path, not through a separate direct-draw path.
- Metal comes first.
- Vulkan follows in a later phase.

## Decision

1. **Tiers.** `vkr_material_graph_lower` first tries the Standard tier. A
   graph with a node only Custom graphs evaluate, or one that the Standard
   tier refuses and the code generator accepts, lowers to a Custom
   definition:
   - `custom_function=vkr_custom_<16 hex digits>`, the FNV-1a hash of the
     generated body, so equal graphs share one function;
   - `custom_param<k>=` for up to 8 exposed constants, so instances change
     row data only;
   - `custom<k>_texture=` and `custom<k>_colorspace=` or `custom<k>_normal=`
     for up to 8 textures;
   - the factors that constants wired straight to the surface output give,
     which the fallback draws with.

   The editor shows the tier, the function and the reason the Standard tier
   refused the graph.
2. **Nodes.** The Custom set adds:
   - inputs: UV, vertex colour, world position, world normal, time, camera
     distance;
   - arithmetic: add, subtract, divide, min, max, lerp, power, one minus,
     saturate, abs, sine, dot;
   - vector nodes: split and combine;
   - UV nodes: tile and offset, and world planar.

   A Custom graph writes base colour, opacity, metallic, roughness,
   occlusion, normal and emissive. Clearcoat, sheen, anisotropy,
   transmission and subsurface stay Standard-only. There is no vertex offset
   and no custom code node (proposal, Settled decisions).
3. **Code generation.**
   [vkr_material_codegen.c](../../runtime/src/assets/vkr_material_codegen.c)
   writes Metal Shading Language directly, because the tiled forward shader
   is MSL. A graph becomes `struct <function> { static VkrMetalCustomOutput
   evaluate(...); }`. Its fragment entries are named `<function>_<Standard
   tiled entry>`. Each instantiates the tiled forward template with
   `VkrMetalCustomSurface<function>` in place of the Standard surface.
   Desktop-pipeline (Slang) generation waits for the Vulkan phase.
4. **Project library.** `vkr_bakery materials --shaders <catalog>` scans
   `assets/**/*.mtg` and keeps the graphs that lower to the Custom tier. It
   writes `metal/project_materials.metal` (the engine's `library.metal`
   followed by every function and its entries) and compiles it with `xcrun
   metal` into `project_materials.metallib`, with `project_materials.json`
   listing the functions and their graphs. An unchanged source skips the
   compile, and a project without Custom graphs has no library. The new
   library replaces the old one only after it links, so a failed compile
   keeps the last good library and a renderer never opens a partial file.
   - A failed compile reports `VKR-MAT-0001`.
   - More graphs than the budget reports `VKR-MAT-0002`.
   - The summary event counts a compile as work, so the editor can tell a
     rebuild from an up-to-date library.
5. **Rows.** Each Metal material row gains a 256-byte Custom segment,
   `VkrMetalPacketCustomMaterial`: 8 texture references, 8 samplers and 8
   `float4` parameters. The frame root reaches it through `custom_materials`
   at byte 576; the root grew to 592 bytes. `custom_time` at byte 292 gives
   the renderer's clock, which wraps hourly. Metal now tracks 30 texture
   references a material, the Custom slots `CUSTOM0` to `CUSTOM7` after the
   layer slots.
6. **Slots and pipelines.** The first time a published material names a
   function, the renderer gives it a slot from 1 to 32 and carries the slot
   in row flag bits 24 to 29. Each published row holds a reference to its
   slot. A background queue then creates the graph's forward and
   alpha-tested pipelines for every tiled variant (lighting, inspection,
   probes and decals): 40 pipeline states a graph.
   `vkr_renderer_get_pipeline_stats` reports the graphs still pending.
7. **Drawing.** Each frame takes a ready mask of the slots whose pipelines
   exist and allocates one `VkrMetalCustomCompaction` table (4,240 bytes)
   in its upload ring.
   - The classify kernel sorts the camera view's draws of ready slots into
     per-slot buckets. It marks them in the classification word with bit 4
     and the slot in bits 27 to 31, which the encode kernel clears.
   - The prefix kernel places each slot's ranges after the Standard buckets,
     within the view's capacity.
   - The tiled opaque pass executes each ready slot's ranges with its
     pipelines in the same opaque-then-alpha-tested order. The picking pass
     draws them with its own pipeline.
   - Shadow views keep Custom draws in the Standard buckets. Alpha-tested
     Custom shadows therefore use the fallback's alpha.
   - The blend pass draws blended Custom materials with their fallback.
8. **Late draws.** A camera draw of a Custom material whose slot is not
   ready draws its Standard fallback and never waits. The classify kernel
   counts it while the slot's pipelines are being created; a slot whose
   creation failed draws the fallback without counting. The count reaches the frame result through the readback slice,
   the `pipelines.late` metric, and the editor's `stats.pipelines_late`
   (total) and `stats.pending_pipelines`. Scene readiness (`wait.scene`, the
   editor's loading state and the harness child) waits until no graph is
   pending.
9. **Edits.** The editor's Bakery daemon watches the content root's
   `assets/` and the catalog's `metal/library.metal`, and reruns `vkr_bakery
   materials` after a change. When it compiled, the runtime asks the
   renderer to reload. A function is named by the hash of its code, so a
   ready graph keeps its pipelines. The next frame preparation in which no
   creation runs:
   1. frees the slots no published row names, after the GPU goes idle, so
      repeated edits do not use up the budget;
   2. opens the new library in place of the old one;
   3. creates again the graphs whose creation failed, such as an edited
      graph the old library lacked.

   A frame that cannot reload yet keeps the request for the next frame.
10. **Budget.** A project holds at most 32 Custom graphs
    (`VKR_MATERIAL_CUSTOM_BUDGET`). Past it:
    - the renderer gives no slot and draws the fallback;
    - Bakery warns (`VKR-MAT-0002`);
    - `vkr_bakery bundle` refuses a catalog whose manifest lists more.

## Consequences

- Instances of a Custom graph share its pipelines. An edit that changes
  only an exposed constant or texture changes row data.
- A new or edited Custom graph shows its fallback for the compile time
  (seconds) and then for the pipelines' creation. On the M1 Pro, creation
  took up to 4.3 s cold and up to 0.16 s warm (Evidence). Other Custom
  graphs keep drawing through a reload.
- Frames without Custom graphs allocate no table, and their roots carry a
  null table, which the kernels test first.
- The editor and a packaged game create the same 40 pipelines a graph:
  leaving the inspection variants out of packages remains.
- Pipelines are not created at cell load: a world-partition cell's Custom
  materials take slots when they publish and draw their fallback until
  ready. The 32-graph budget is a constant, not a project setting, and
  `art.lint` does not check it yet.
- Vulkan, a blended Custom material and an alpha-tested Custom shadow draw
  the fallback.
- Eight more texture slots enlarge every in-flight material load to about
  56 KB, so the material system reserves 32 MB for them.

## Evidence

All runs on 2026-10-09: Release, AppleClang 21, Apple M1 Pro, Metal 4, a
headless Bistro editor on the tiled pipeline. Brushes above the Bistro
streets were bound by a surface theme to Custom graph instances
(`.scratch/artist/phase4_evidence.py` in the working tree).

- **CPU suites.** `./build_release/tests/vulkan_renderer_tester --suite
  <name>`: 98 suites pass. The excluded `run_local_socket_tests` fails the
  same way on `main`.
  - `run_material_graph_tests`, `test_graph_custom`:
    - lowering to the Custom tier;
    - a function hash stable under renames and moves and changed by a
      constant;
    - texture classes;
    - the loader round trip;
    - rejections of mixed components and of clearcoat.
  - `run_metal_packet_abi_tests` pins the 592-byte frame root.
- **Equal look.** A Standard graph (grid texture × tint, roughness 0.6,
  metallic 0) was compared with a Custom graph that adds an identity tile
  and offset, bound in turn to one platform and wall.
  - Unlit, the brush region (700×260 pixels) is bit-identical (maximum
    difference 0).
  - Lit, the Standard and Custom captures differ by a mean of 1.55 per
    channel. That is below the 3.60 of two Standard captures of the same
    camera, because moving clouds change the lit image.
- **Eight graphs.** Eight Custom graphs were bound to eight boxes at once,
  followed by a 240-step `view.camera` glide past them.
  - `stats.pending_pipelines` was 0 when `wait.scene` returned.
  - `stats.pipelines_late` stayed at its value from the binding (148 draws
    warm, 5,752 cold) through the glide: zero late draws.
- **Creation time** (pipeline cache from the earlier run, else cold):

  | Case | Cold | Warm |
  |---|---|---|
  | One graph alone | 3.6 s | 31 ms |
  | Seven graphs in parallel, each | 4.1 to 4.3 s | 127 to 164 ms |

- **Edit.** An agent made a new Custom graph with `material.create` and
  `material.patch` in a running editor and bound it by theme
  (`.scratch/artist/phase4c_check.py`).
  1. The graph drew its fallback (its function was not in the library).
  2. The Bakery watch compiled the library.
  3. The renderer reloaded it, and the graph was ready 3.4 s later with
     its 3×-tiled grid.
  4. A second `material.patch` of the tiling made a new function. The next
     reload freed the first one's slot ("1 unused freed") and created the
     second.
  5. Appending a comment to the catalog's `library.metal`, then restoring
     it, recompiled and reloaded the library each time.
- **Validation.** The eight-graph run under Metal API validation
  (`MTL_DEBUG_LAYER=1`, one process) reported no error and shut down
  cleanly.
- **Timing.** `vkr_harness profile --case
  tools/cases/performance/bistro_material_orbit_1440.case.json --profile
  tools/profiles/performance-windowed-gpu-submission.json`, Bistro without
  Custom graphs, so the change under test is the Standard path's share:
  - Configuration: Release, AppleClang 21, Apple M1 Pro, Metal 4,
    2560×1440 pixels, windowed hidden, immediate present; five children of
    120 warmup and 300 measured frames each side.
  - Before: commit `60f1522d`, built in a separate worktree. After: commit
    `33db15da`. The four runs alternated after, before, after, before under
    heavy background load (load average 12 to 40).
  - All four passed and are authoritative, with equal environment,
    workload and policy fingerprints.
  - `gpu.submission` mean: before 20.440 and 20.948 ms, after 20.447 and
    20.531 ms; per-child spread 20.0 to 21.5 ms on both sides. No change
    beyond the spread.
  - Reports: before
    `sha256:1eaa8caa672c6c656ff10e5f2b8fb82d1a470e34f62163f04e2d8729669d0ed9`
    and `sha256:fdfbbac96a241d14d736b4aebc4bf9f0c23cee7d7f661564bee3260e3f4c707d`;
    after
    `sha256:f841a5695bac934e686ab2215a2d19b74039b0952681e7d9fd7a0a542e4fc848`
    and `sha256:824dcc89c0dbf57a2f8a4f3a3d25206ecb1d50e0ae970b41856ac3ca78499000`.
  - The first after-runs failed the work-volume check: 32 texture slots
    grew each in-flight material load to 56 KB, and Bistro's 254
    concurrent loads overflowed the material system's 16 MB async reserve,
    so some materials failed to load. `33db15da` raised the reserve to
    32 MB.
- **Unavailable:**
  - Vulkan native execution: Vulkan draws the fallback.
  - Register counts of the Custom variants.
  - A world-partition cell with Custom graphs.

## Alternatives considered

- **A direct-draw path for Custom materials.** Rejected (owner,
  2026-10-09): it would lose GPU culling and LOD for those draws.
- **Slang generation cross-compiled to MSL.** Rejected for the tiled
  pipeline: its forward shader is MSL, and the template instantiation keeps
  one shading body.
- **Waiting for pipelines at draw time.** Rejected: a late pipeline must
  never stall a frame (proposal, Part 4).

## Revisit when

- The Vulkan phase adds classification and per-graph resolve.
- Packaged games need fewer pipelines: drop the inspection variants.
- World partition needs cells to wait for their Custom pipelines.
- Measured creation time on other GPUs calls for a different budget.
