# Documentation index

[ARCHITECTURE.md](ARCHITECTURE.md) describes the current renderer and its known
boundaries. [CONTEXT.md](CONTEXT.md) defines project vocabulary. Code and its
production callers are the implementation authority; ADRs explain decisions,
and proposals describe future work.

This inventory covers every retained document. ADR numbers remain stable;
removed numbers are not reused.

## Build and run

Clangd uses `build_release/compile_commands.json`. Run `./build_release.sh` or
`./build_editor.sh Release` after source moves or build-definition changes to
refresh its include paths and compiler settings.

The build wrappers configure all application, editor, tool, test and example
consumers in one tree per configuration: `build_debug`, `build_release`,
`build_release_info`, or `build_min_size_rel`. They build only the selected target
and its dependencies; app and editor wrappers also select `vkr_harness`.
Switching targets reuses the existing library objects, shader outputs and CMake
cache. Compiler, generator and toolchain selections remain attached to that
cache. The Xcode wrapper also reconfigures its existing `build_xcode` tree.

```sh
./build_release.sh
./build_editor.sh Release
./build_lib.sh renderer
./build_lib.sh runtime
```

`renderer` builds [examples/renderer/main.c](../examples/renderer/main.c) against
`renderer_lib`; `runtime` builds [examples/host/main.c](../examples/host/main.c)
against `vkr_runtime`. Both reuse `build_release` without building the other
consumers. On Windows, use the matching `.bat` wrappers. Custom CMake builds may
still disable optional consumers with `VKR_BUILD_APP`, `VKR_BUILD_EDITOR`,
`VKR_BUILD_TOOLS`, `VKR_BUILD_HARNESS`, and `VKR_BUILD_TESTS`. Use a separate tree
for that restricted graph; repository wrappers enable these options again.
`VKR_BUILD_RUNTIME=OFF` also removes the runtime when its consumers are disabled.

The app and editor are separate executables using `vkr_runtime` and
`vkr_sample_runtime`. Repository builds compile `vkr_bakery` and run
`vkr_bakery shaders` into `<build>/shaders`, and cook only the tracked
textures the engine content, default scene, fixture scenes and CPU tests read
(`vkr_engine_textures`); they cook no other asset. Textures are host-native
(ASTC on Apple silicon, BC on x86-64) and untracked, so a fresh checkout cooks
downloaded scenes such as Bistro with `vkr_bakery build assets/bakery.json`
before rendering them ([ADR-012](adr/012-texture-compression-pipeline.md)). `build_test.sh` and
`build_test.bat` build and run the CPU tester in `build_debug` by default.

Set `VKR_DEBUG_SANITIZER` to `default`, `address`, `thread`, `memory`, `leak` or
`none` before invoking a wrapper. Explicit profiles select
`build_debug_<profile>` and require Debug; `default` or an unset value uses
`build_debug`. App, editor, tool and test consumers reuse dependencies within
the same profile. `VKR_BUILD_DIR` overrides the selected directory. The Xcode
wrapper uses `build_xcode_<profile>` for explicit profiles, with instrumentation
limited to its Debug configuration.

```sh
VKR_DEBUG_SANITIZER=thread ./build.sh Debug
VKR_DEBUG_SANITIZER=thread ./build_editor.sh Debug
VKR_DEBUG_SANITIZER=thread ./build_test.sh
VKR_DEBUG_SANITIZER=leak ./build_test.sh
```

Use these examples only on toolchains supporting the selected runtime; see the
[profile and platform limits](ARCHITECTURE.md#build-policy) and
[sanitizer validation procedure](../.codex/skills/vkr-validation/SKILL.md#cpu-sanitizers).
For Windows wrappers, set the environment in PowerShell, for example
`$env:VKR_DEBUG_SANITIZER = 'none'; .\build.bat Debug`. Windows wrappers
normalize profile case; shell wrappers require the lowercase values above.

Release compiles INFO, WARN and ERROR logging for both app and editor and
strips DEBUG/TRACE; the app prints errors only unless it raises its threshold,
and the editor captures INFO for its Console and session log. Set
`VKR_EDITOR_LOGGING=ON` in the environment before a build, or set the CMake
option explicitly, to compile DEBUG/TRACE as well. This changes shared
library compilation and remains in the cache until explicitly set to `OFF`.
Selecting app versus editor does not change it. The
[build policy](ARCHITECTURE.md#build-policy) defines optimization and dependency
configuration; these settings alone do not establish measured performance.

[`vkr_bakery`](adr/077-asset-build-system.md) owns every cooked asset, table,
shader catalog, project job, project package and scene bake; the editor's Build
menu and its Develop > Bakery panel drive it,
and on macOS the editor keeps one `vkr_bakery serve` daemon for file watches.

When artifact regeneration is required, run Bakery or `vkr_bakery` directly:

```sh
./build_release/tools/bakery/vkr_bakery cook assets/models/bistro.gltf
./build_release/tools/bakery/vkr_bakery build assets/bakery.json
./build_release/tools/bakery/vkr_bakery help
```

`vkr_bakery bundle assets/bundles/bistro.bundle.json --out <dir> --app
build_release/app/vulkan_renderer --shaders build_release/shaders` writes a
Bistro bundle (about 2.5 GiB) whose executable runs from any directory.
`vkr_bakery bundle <workspace>/.vkreditor/projects/<id> [--profile <name>]
[--out <dir>]` packages a managed project with the `vkr_player` template that
every build tree places in `<build>/player`; the editor's Build menu runs the
same command ([ADR-078](adr/078-project-build-and-packaging.md)).

Unchanged inputs hit the per-user cache instead of re-encoding. The main Bistro artifact includes its scene-specific light
ranges; runtime mesh loading accepts `.vkb`, not source OBJ/glTF/GLB. Small
tracked harness scenes use cooked fixtures under `tests/fixtures/rendering`.


On Windows, use `build_release.bat` or `build_editor.bat Release`. Set
`VCPKG_ROOT` to your vcpkg checkout and install the font cooker's dependency:

```powershell
& "$env:VCPKG_ROOT/vcpkg.exe" install freetype:x64-windows-static
.\build_release.bat
```

CMake uses that checkout's toolchain and defaults to `x64-windows-static`,
including its static Release C runtime in every configuration. Debug VKR code
retains its own assertions and debugging information; imported dependencies use
their Release variants. Explicit toolchain and triplet settings take
precedence. Use a fresh build directory when changing either setting.

`build_run.sh` and `build_editor_run.sh` also launch their respective targets.
`vkr_bakery` cooks an explicitly selected animation bank or static collision
proxy, and `tool <name>` runs a cooker's own command line:

```sh
B=./build_release/tools/bakery/vkr_bakery
$B cook /path/to/player.gltf --producer animation --out /path/to/player.vka
$B tool animation --inspect --input /path/to/player.vka --clip 0 --time 0.5
$B cook /path/to/proxy.gltf --producer collision --recipe kind=hull --out /path/to/proxy.vkc
$B tool collision --inspect --input /path/to/proxy.vkc
```

Use `kind=mesh` for Static/Kinematic triangle collision and `node=<index>` for
a selected subtree in its local coordinates. Bakery exposes both recipes.
[ADR-072](adr/072-entity-collision-and-rigid-body-physics.md) specifies format limits.
Managed model import and Rebuild publish matching mesh and animation banks.
Scene bindings evaluate per-wrapper poses and blend controllers for compute skinning; see
[ADR-071](adr/071-animation-bank-and-reference-pose.md).

The default mannequin is generated, checked and published explicitly; the
build wrappers never run Blender
([ADR-080](adr/080-default-mannequin-character.md)):

```sh
BLENDER=/Applications/Blender.app/Contents/MacOS/Blender
python3 tools/blender/mannequin/fetch_sources.py <sources>
$BLENDER --background --factory-startup \
  --python tools/blender/mannequin/build_mannequin.py -- \
  --sources <sources> --out <build> --blend
$BLENDER --background <build>/mannequin.blend \
  --python tools/blender/mannequin/check_mannequin_motion.py
python3 tools/blender/mannequin/publish_mannequin.py --build <build> \
  --bakery build_release/tools/bakery/vkr_bakery
```

For profiles, captures, and baselines, use the
[harness workflow](../.codex/skills/vkr-harness/SKILL.md). Use normal Release
with graphics validation variables unset for performance and baseline evidence;
use [native validation](../.codex/skills/vkr-validation/SKILL.md) only for a focused
diagnostic. Metal execution does not establish native Vulkan compatibility.

## Decisions in force

These records are source-audited implementation decisions, not a declaration
that every platform, quality, or performance acceptance gate has passed. Each
record identifies its code owner and any remaining integration or evidence gap.

| ADR | Decision | Status |
|---|---|---|
| [002](adr/002-render-graph.md) | Declared frame dependencies and JSON topology | implemented |
| [004](adr/004-stateless-render-packet.md) | Explicit frame inputs, application ownership and acquired frame context | implemented |
| [006](adr/006-cpu-memory-allocators.md) | CPU allocation by lifetime | implemented |
| [009](adr/009-frame-synchronization.md) | Separate submission and presentation completion | implemented |
| [010](adr/010-ecs-scene-system.md) | ECS-owned scene state with glTF node identities and a retained render mirror | implemented |
| [012](adr/012-texture-compression-pipeline.md) | Host-native KTX2 texture artifacts cooked per host and loaded as stored, with no transcode, source fallback or tracked `.vkt`: ASTC (6x6 colours and data masks, 4x4 normals) on Apple silicon and BC7/BC5 on x86-64, from the system encoder for editor-only textures; a runtime texture load limit, 2048 by default on Metal, and Graphics and per-scene limits applied live to material textures by bounded reloads | implemented |
| [014](adr/014-offscreen-present-target.md) | Window and offscreen targets share frame submission | implemented |
| [015](adr/015-metrics-module.md) | Bounded typed metrics and pinned snapshots | implemented |
| [017](adr/017-prepared-specular-glossiness-lowering.md) | Prepare PBR materials before publication | implemented |
| [018](adr/018-graph-declared-transmission-feedback.md) | Ordered transmission with declared feedback (desktop pipeline, Vulkan) | implemented |
| [019](adr/019-bounded-forward-spatial-lighting.md) | Bounded punctual lighting with a 128-light table ranked by camera distance; cached local shadows in an atlas with a layer budget per preset, static squares, and dynamic-caster copies reused while the casters stand still; reflection probes with inward blend bands, smaller-box-first priority, their own gains, dominant-direction lookups and mip-chained, supersampled baked cubes of the static world captured without sky discs, glow or stars, whose brightness follows the time of day by the diffuse volume's light at their capture origin, and Bistro's café probe; the frustum-gated mask, contact shadows, transmission layers and contribution ranking on the desktop pipeline (Vulkan) | implemented |
| [023](adr/023-vulkan-1-4-bindless-capability-profile.md) | One explicit Vulkan capability floor | implemented |
| [024](adr/024-shared-bindless-gpu-cores.md) | Shared allocation/completion cores and bounded Metal heap residency | implemented |
| [025](adr/025-selected-renderer-implementation-strategy.md) | Procedural renderer and prepared native commands | implemented |
| [027](adr/027-immediate-mode-grid-ui.md) | Content-sized grid UI, viewport bars/world grid, dock stacks, Console and Bakery | implemented |
| [028](adr/028-gpu-driven-deferred-visibility-buffer.md) | One GPU-driven world topology: shared culling and indirect draws; visibility buffer and deferred resolve on the desktop pipeline (Vulkan); desktop culling rows per candidate with exact bucket partitions | implemented |
| [029](adr/029-retained-graph-resources.md) | Retain submitted image contents per subresource | implemented |
| [030](adr/030-offline-mesh-optimization-and-cooking.md) | Versioned meshoptimizer artifacts preserving glTF nodes, shared geometry and CPU skin influences | implemented |
| [031](adr/031-versioned-packed-static-geometry-abi.md) | One 32-byte packed static vertex ABI | implemented |
| [032](adr/032-two-phase-confirmed-visibility.md) | Keep exact one-phase visibility gates | declined |
| [033](adr/033-occupied-depth-sdsm-feedback.md) | Optional occupied-depth shadow fitting (desktop pipeline, Vulkan) | implemented |
| [034](adr/034-offline-cooked-font-artifacts.md) | Cooked MTSDF font artifacts | implemented |
| [035](adr/035-canonical-mtsdf-screen-pixel-range-shading.md) | Derivative-based MTSDF coverage | implemented |
| [036](adr/036-dpi-derived-ui-text-scale.md) | Window content scale before UI layout | implemented |
| [037](adr/037-portable-same-resolution-temporal-antialiasing.md) | SMAA 4x and T2x on the desktop pipeline (Vulkan): two-subsample jitter, SMAA after the tonemap, a two-frame resolve with velocity weighting and an identity/depth accept mask for moving pixels, and the immediately preceding frame as the history of T2x and motion blur; opt-in S2x on a 2x multisampled visibility raster with the second sample shaded at listed edge pixels; replaced portable TAA and FXAA on 2026-10-13 and the visibility-buffer MSAA proposal on 2026-10-15 | partial |
| [038](adr/038-sh-l2-diffuse-irradiance.md) | GPU-resident L2 diffuse response | implemented |
| [039](adr/039-metal-internal-render-scale.md) | Separate internal Scene and physical output extents; the Metal Scene scale that tiled adaptive quality steps | implemented |
| [041](adr/041-retained-cascaded-shadows.md) | Stable fits and retained directional shadow cascades; a shared static layer per cascade that dynamic casters are drawn over after a copy, reused while they stand still | implemented |
| [042](adr/042-scene-linear-post-processing.md) | Scene-linear exposure with completed-history time and bloom on both pipelines; directional ambient visibility (GTAO) on the desktop pipeline | implemented |
| [043](adr/043-presentation-dpi-and-color-transfer.md) | Physical-pixel presentation, the macOS high-DPI switch, color transfer and image sharpness; FXAA only on the desktop pipeline | implemented |
| [044](adr/044-shader-cross-backend-contract.md) | Portable shader semantics and native ABI validation; shared, desktop-only and tiled-only shader domains with native evidence per backend (no class has two backends, so no domain is ALIGNED); editor inspection modes | implemented |
| [045](adr/045-resource-prepare-and-render-thread-finalize.md) | Worker preparation and render-thread resource finalization | implemented |
| [046](adr/046-editor-viewport-mapping-and-picking.md) | Perspective/orthographic viewport mapping, axis, plane and center gizmo handles in world or local space with per-axis scale, selection outline, picking and retained Scene presentation | implemented |
| [047](adr/047-event-payload-and-resize-mailbox-lifetimes.md) | Event callback payload lifetime and coalesced resize handoff | implemented |
| [051](adr/051-renderer-harness-and-evidence.md) | Isolated harness runs, warmup aligned to the raster jitter and cloud march noise phases (camera script version 8), workload fingerprints that record the pipeline class, reconstructed HDR diagnostics and reviewed capture baselines | implemented |
| [052](adr/052-vulkan-fsr31-upscaling.md) | Vulkan FSR 3.1 native antialiasing at render scale 1, the desktop default since 2026-10-15 with SMAA T2x as fallback; the 2026-10-12 removal of the upscaler and why it returned | partial |
| [053](adr/053-energy-compensated-ggx.md) | Correlated Smith GGX and shared energy integration | implemented |
| [054](adr/054-baked-diffuse-volumes.md) | Sparse baked diffuse volumes: 4×4×4 probe bricks over three levels dense near surfaces, probe relocation and distance moments, L1 SH layers per sun key and lamp group with lamp direct bands, `DVOL` v3, the Vulkan and Metal probe gathers and their CPU reference, a warned skip for scenes over the brick budget, GPU composition, the desktop pipeline's half-resolution lookup with exact fallback, the tiled pipeline's inline lookup with direct bands in its single sum, bands clamped non-negative, the global environment's reflection share scaled by baked sky light (sun-key lightmap layers or volume light against the environment's SH) on both pipelines while probes keep their captured radiance, the Bistro scene's volume, and the Bistro evidence on both | partial |
| [055](adr/055-screen-space-reflections.md) | Opaque screen-space reflections, removed on 2026-10-12 for noise and temporal artifacts; probes and the global environment reflect instead | declined |
| [056](adr/056-rectangular-ltc-lights.md) | One-sided rectangular LTC emitters, runtime lookup and baker transport | implemented |
| [057](adr/057-analytic-height-fog.md) | Analytic scene-linear height fog with ordered transmission composition, applied by both pipeline classes | implemented |
| [058](adr/058-revision-baked-sky-atmosphere.md) | Atmosphere as the only sky, camera-dependent sky and aerial perspective, sky-light controls, constant fixture source and unified sun | implemented |
| [059](adr/059-froxel-volumetric-fog.md) | Bounded volumetric fog with independent scattering history (desktop pipeline, Vulkan) | implemented |
| [060](adr/060-screen-space-diffuse-indirect-lighting.md) | Screen-space diffuse indirect lighting (SSGI), removed on 2026-10-12 for noise and temporal artifacts; lightmaps and diffuse volumes supply diffuse bounce | declined |
| [061](adr/061-extended-linear-display-output.md) | Optional EDR/scRGB output with OS headroom and SDR fallback | implemented |

| [062](adr/062-layered-clearcoat.md) | Independent clearcoat maps and layered GGX; the coat lobe reflects probes and the environment like the base lobe (desktop pipeline, Vulkan) | implemented |
| [063](adr/063-charlie-sheen.md) | Charlie sheen, bounded energy and shared rectangle tables (desktop pipeline, Vulkan) | implemented |
| [064](adr/064-anisotropic-ggx-reflection.md) | Anisotropic GGX reflection, directional tables and material transport (desktop pipeline, Vulkan) | implemented |
| [065](adr/065-thin-sheet-diffuse-transmission.md) | Thin-sheet diffuse backlighting and offline transport (desktop pipeline, Vulkan) | implemented |
| [066](adr/066-post-reconstruction-depth-of-field.md) | Optional post-reconstruction lens blur (desktop pipeline, Vulkan) | implemented |
| [067](adr/067-post-reconstruction-motion-blur.md) | Optional camera and rigid-object shutter blur (desktop pipeline, Vulkan) | implemented |
| [068](adr/068-profiled-surface-diffusion.md) | Optional RGB surface diffusion and offline transport (desktop pipeline, Vulkan) | implemented |
| [069](adr/069-editor-projects-and-workspaces.md) | Portable workspaces, Blank/FPS Arena/RPG Grounds starter projects, background preference publication, per-machine editor settings and scene recall in `.vkreditor/local` with only the animation graph shared in `project.json`, depot clones that initialize their workspace, managed imports, entity addition, scene publication and deletion, and the scene opening state shown until a scene is usable | implemented |
| [070](adr/070-portable-path-boundaries.md) | Native UTF-8 I/O, managed reference grammar, format boundaries and path regression gates | implemented |
| [071](adr/071-animation-bank-and-reference-pose.md) | Animation banks, CPU playback, compute deformation and the movable graph/sequence preview editor | partial |
| [072](adr/072-entity-collision-and-rigid-body-physics.md) | Scene-owned Jolt bodies/joints, cooked collision, static height fields, bone attachments, queries and transactional editor authoring, with steps of 512 or more active bodies spread over the engine's job workers | implemented |

| [073](adr/073-native-gameplay-foundation.md) | Shared scene ticks independent of input focus, ordered input, C player/weapon client (now the FPS script module) with an authored camera mode used by the default project scenes, action animation, native character stance and camera rigs; general visual/prefab authoring pending | partial |
| [074](adr/074-volumetric-cloud-layer.md) | One volumetric cloud layer: runtime-generated noise, half-resolution traced history, sun-projected shadows and aerial perspective at cloud depth | implemented |
| [075](adr/075-editor-cmd-bar-and-evaluator.md) | Editor Cmd bar: typed commands with completion, an expression evaluator over scene and editor data, and `--exec` scripts with `[cmd]` stdout results | implemented |
| [076](adr/076-project-object-model.md) | Typed descriptors, entities as ID plus components, the World beside primary and additive scenes with inherit-World singleton resolution and Set primary, World-only physics and animation settings, shared physics, structural undo, presets, registered component types, document-stable entity IDs, version 5 scene documents, prefab instances, Content folder browser, Outliner above Details, viewport documents, an asynchronously loaded World, a primary-scene overlay read on a worker and applied over frames, and import placement in the World, a new scene or a project scene with cooking marks | implemented |
| [077](adr/077-asset-build-system.md) | One `vkr_bakery` program for cooked assets, tables, the shader catalog with Metal metallibs, managed project jobs, scene bakes and material previews, with one action cache, scheduler and event stream, the editor's `serve` daemon for shader and Content file watches, `.vkpak` version 2 bundles with zstd-compressed in-memory chunks mounted under one content root (the repository or an installed `content/`), C script packages and project libraries, deferred, preview and final texture tiers, a fast editor encode speed and progressive finalization in the editor, platform-neutral build texture names with recipes and `recook_textures`, `share_project`, and editor material writes into a shared workspace's own assets; Windows paths pending | partial |
| [078](adr/078-project-build-and-packaging.md) | Managed projects build into standalone games: `game.json` settings, profiles and an authored startup camera, portable lowering, project-mode `vkr_bakery bundle` with the project's script library, staged publication and a report, `vkr_player` templates with windowed, fullscreen and borderless modes, signed macOS `.app` packages carrying the Vulkan loader, `bundle.json` version 2, reuse of unchanged archives, the editor Build menu, settings, progress, diagnostics revealed in Content and `build.*` commands with Bakery under Develop, and a relocatable editor distribution from `build_editor_dist.sh` | implemented |
| [079](adr/079-c-script-modules.md) | C script modules through `sdk.h` over a host-owned table taking `VkrStr` string views, with temp, scoped and container-persistent lifetimes released through ledgers, owner and timed lifetimes, structural edits in fixed updates queued until the tick ends, script tasks on worker threads, per-entity behaviors with destroy hooks and script instances per attached container, the runtime script host with Play/Stop sessions, shared-library loading with state-keeping hot reload prepared on workers that migrates changed component fields, models spawned by scripts loading on workers, project `Scripts/` packages, modules and libraries with dependencies, built by Bakery into one project library that loads before the project's documents (a first build runs on a worker), the floating Script editor with C highlighting, completion and diagnostics, Script assets attached to objects by drag or the per-entity script slot, authoring macros for components, behaviors and modules, the Player Start and its capsule, World-only Play, and the FPS sample as a statically linked module, and packaged games that load the project's script library; exports and TypeScript pending | partial |
| [080](adr/080-default-mannequin-character.md) | The default mannequin: a UE5-style armoured character generated in Blender from pinned CC0 and CC BY sources, with baked PBR textures and 18 posture-corrected motion-capture clips, published as engine content with its credits, spawned under a wrapper by `vkr_scene_spawn_model`, the player's body unless a placed model carries `fps_player`, driven by speed-synchronized locomotion posed at render time, with third-person orient-to-movement | implemented |
| [081](adr/081-physical-night-sky.md) | Physical night sky: whole-stop pre-exposure with exact history rescaling and night-range metering, the moon as a second atmosphere light with its own sky-view table, phase-shaded disc and key-light rule, and a procedural star field that turns with the sun about the celestial pole | implemented |
| [082](adr/082-renderer-owned-render-thread.md) | Default renderer-owned render thread: submit and complete, render-thread acquisition of decoupled frames with frame hooks and render-side shadow resolution, coupled frames for resizes, an ordered asset-publication queue with completions and confirm-before-use, waits at every other renderer call, and copies of update-mutable packet data | implemented |
| [083](adr/083-supported-hardware-matrix.md) | Supported hardware matrix: Apple M1+ with Metal 4 (tiled pipeline), AMD RDNA 2+ and NVIDIA Ampere+ with Vulkan 1.4 on Windows (desktop pipeline), their capability differences (formats, subgroup width, atomics, mesh shading, ray tracing, memory model, scene scaling), the 16 GB Mac / 8 GB GPU memory floor for production Bistro, and rules for using the matrix in performance and graphics work | partial |
| [084](adr/084-agent-channel-and-level-design-toolkit.md) | Agent channel: a per-user socket of typed editor operations with JSON Schemas, batches applied as one journal group with rollback, journal groups with grouped undo and out-of-order revert, designer review in the Agent changes window with Scene outlines, Scene and window PNG captures, the Cmd bridge, headless runs kept open for agents, request authors with agent-scoped undo, reads that wait for rebuilds (`settle`), quick reads sharing a build, `level.map` text floor plans and terrain height grids, region claims that outlive a restart, the change feed with waiting reads, MCP subscriptions and a watcher for background monitors, `entity.place`, capture sheets with labelled marks that report occlusion and that wait for an idle designer, level checks spread over builds, the `vkr_mcp` stdio adapter for MCP 2026-07-28 only; Hammer-style brushes with faces as child entities, Hammer UVs, surface tags and marks with fixed greybox looks, the greybox view and measurement aids, generated meshes and per-cell generated collision rebuilt within a per-update count and a 6 ms budget, brush and blockout operations, the Level Create group and brush drawing; brush editing (face moves, extrude, clip, hollow, carve, merge, the face grid with patch push and pull and corner, edge and grid-line reshaping, face handles and the clip tool) and level checks against the player capsule with the Level checks window; entity IO (outputs, inputs and connections from engine components and script behaviors, the router, trigger hooks, Details sections and Scene lines, `io.*` operations); heightfield terrain with sculpt and paint, region operations, tile meshes, the four-layer terrain material and height field collision; population (splines through child points, spline meshes and seeded scatter as runtime mesh instances under a 4,096-copy bound, `terrain.road`) | partial |
| [085](adr/085-gpu-geometry-lod-and-terrain-geomorphing.md) | GPU geometry LOD table: per-range levels in decode metadata, per-view selection during culling, level ranges and LOD state in visible rows; terrain tiles' seven levels with geomorphing in raster, resolves, transmission and motion vectors; cooked static mesh levels simplified per range with locked borders | implemented |
| [086](adr/086-world-partition.md) | World partition: streaming sources, tiled heightfields up to 8,192 cells with a tile window, overview tiles with holes and one windowed collision body; `world_partition` cells of editor-created roots, per-cell documents and the cell index; budgeted cell streaming with the unload rules, Play holds and IO router refresh; baked brush proxies; origin rebasing during Play with exact restore; the World Partition window and `partition.*` operations; the measured local-shadow hitch over terrain | partial |
| [087](adr/087-gpu-class-graphics-pipelines.md) | Graphics pipelines per GPU class: a tiled pipeline for tile-based GPUs (M-series, later mobile) and the desktop pipeline for discrete GPUs; the class follows the backend (Metal tiled, Vulkan desktop) with no selection; a shared art-level contract; the M1 Pro budget of 2560×1440 at 16.7 ms p95 without upscaling; the tiled pipeline's graph, multisampled forward pass, lightmaps, glass, dynamic lights, unbaked static lights, terrain blending, editor and inspection modes, atmosphere and fog, and adaptive quality with its dynamic-resolution controller; Bistro measurements | partial |
| [088](adr/088-baked-lightmap-sets.md) | Baked lightmap sets: lightmap UVs on cooked and managed models, brushes and blockout shapes, world-density packing, eight sun keys, light mobility and up to four named lamp groups, the Metal and Vulkan ray-query bakers and their CPU parity, the punctual shadow clip, indirect-light denoising and buried-texel fill, lamp direction pages, VKLM v6 plane tables written per host platform, `vkr_bakery bake lightmap`, project storage with one set per pipeline class in a scene and the Bakery panel options; runtime loading, draw binding and tiled-pipeline sampling; a check on a baked scene and a CPU bake path pending | partial |
| [089](adr/089-editor-workbenches.md) | Editor workbenches: a tab row under the top bar, the Views panel switching the Scene, the selection alone on a neutral model stage scene (Model, with its own orbit), the material Graph and the Script editor with the Scene unrendered under the last two, scrolling and folding Tools palettes, the active tab named after the open scene the Scene panel shows (no document tabs) and each tab keeping its own project scene, switching General, Level Design, Terrain, Lighting, Scripting and up to nine with custom copies (duplicate, rename, move, delete), each a dock layout, open windows and Scene mode; shared palettes over existing commands and operations, two-click stairs and corridor tools, brush roles, doorway, the Scene tool hint, multiple selection by Ctrl+click with Merge, Duplicate and Delete acting on all of it, warm assets that keep the closed scene's meshes, materials and textures for a switch back, docked Level checks, Script editor and Terrain panels with one host per body, Ctrl+1..9 and Ctrl+PageUp/PageDown, the tab menu, Cmd `workbench*`, `ui.workbench` and `workbench.*` operations, and persistence in per-machine project settings and the layout file | partial |
| [090](adr/090-time-of-day.md) | Time of day: the World-only `time_of_day` clock advancing with the simulation, the sun and moon turned about the celestial pole from their noon directions, the night fade, light group factors scaling static lights, diffuse-volume layers and the tiled pipeline's lamp layers, and the script SDK and `time.hour` / `light.group` controls | implemented |
| [091](adr/091-containers-array-and-hash-table.md) | Containers: one growable `Array` (reserve or filled creation, element-aligned storage, doubling growth, slot tables that never grow) replacing `Vector`; the hash table with power-of-two capacity, stored hashes, an fmix64-finished FNV-1a index, counted tombstones purged at the same capacity and no probe limit; the codepoint-sorted legacy glyph index | implemented |
| [092](adr/092-projected-decals.md) | Projected decals: the `decal` world component (a box from its entity's transform, projecting along its -Y, a `.mt` material, opacity, angle and depth fades, sort order), per-scene material references, the 64 nearest decals per frame in compositing order with a world grid shared in sizing with the point-light grid, tiled forward shading that lays their base colour before lighting in decal shading variants, and the same blend in the desktop G-buffer resolve; extra channels and an editor box outline pending | partial |
| [093](adr/093-material-graphs-and-art-workbench.md) | Material graphs (`.mtg`) with a node registry, Standard lowering to `.mt` definitions (no shader, no pipeline), graph instances the material loader and lightmap baker expand, Bistro materials raised and lowered exactly; the removed `shader=`/`pipeline=` keys; the editor's material documents with a journal ordered with scene edits and agent review, the node canvas, the Material panel and the Art workbench; `material.*` operations and the vkr-art skill; layered and Custom tiers in ADR-095 and ADR-096 | partial |
| [094](adr/094-surface-themes-and-art-pass.md) | Surface themes (`.surfaces`) binding tags to materials per container over the World's, the look order, material `world_size` and `surface` keys sizing brush-face UVs, rebuilds on theme and size changes, bakes reading the scene's theme, the theme table and Art palette rows, `surface.*` operations and `art.lint`; per-region themes pending | partial |
| [095](adr/095-layered-standard-materials.md) | Layered Standard materials: the terrain blend generalized to vertex colour, mask texture, slope and height masks in the shared kernel, grown layer segment rows, `layer1=` to `layer3=` and mask keys composed by the loader, layer and layer blend graph nodes, bakes as layer 0, and the matched Bistro timing at 2560x1440 | implemented |
| [096](adr/096-custom-material-graphs.md) | Custom material graphs: the Custom node set and tier, MSL code generation per graph, the project library from `vkr_bakery materials`, 256-byte Custom row segments and a 592-byte frame root, per-graph pipeline slots created in the background, per-graph camera buckets in the GPU draw kernels, the Standard fallback with `pipelines.late`, readiness waits, reload through the Bakery daemon and the 32-graph budget; Metal only, Vulkan draws the fallback | partial |
| [097](adr/097-look-volumes.md) | Look volumes: the `look_volume` component (a transform box with priority, blend distance and overrides of exposure, metering range, white balance, contrast, saturation, bloom, height fog colour and density and sky light), the CPU weight and priority blend at the camera feeding the frame globals, frame input 53's metering range, the Lighting palette entry and `look.volume` | implemented |
| [098](adr/098-environment-panel-and-presets.md) | The Environment window (each environment part with its source, Scene, World or unset, edited in place, Add and Override in scene) and environment presets (`.environment` documents of up to nine parts, saved through the document journal and applied by copy as one batch), `env.describe`, `env.preset.save` and `env.preset.apply` | implemented |
| [099](adr/099-artist-views-and-luminance-queries.md) | Artist views on the tiled pipeline (base colour, roughness, metallic, normals, material cost, texel density as data views shown without exposure or tone curve, and exposure false colour by stops in the tonemap), shared ramps in `editor_view.slangh`, `view.capture` `mode`, and `query.luminance` over the HDR capture; desktop views pending | partial |
| [100](adr/100-lighting-tools.md) | Lighting tools: bake settings sent with Bake lighting (lightmap and diffuse groups, zero keeps Bakery's default), the Lighting palette's time scrubber and Keep hour, world-space outlines of selected lights, look volumes, decals and reflection probes, the Lights window (live group sliders, the selected light's rows, every light with enabled, mobility, group and intensity) and `lighting.list`, `lighting.group`, `lighting.time` and `lighting.bake` | implemented |
| [101](adr/101-decal-placement.md) | Decal placement: the orientation rule (+Y along the normal, the image's top up a wall or along the view on a floor, then a turn), the Art palette's Decal Scene tool with size, depth and turn, `decal.place` at a point or a ray hit, and `editor.status` `view.image` for scripted Scene clicks | implemented |
| [102](adr/102-scatter-painting.md) | Scatter painting: `scatter_area` children (radius, density, spacing, seed, order) that replace a scatter's box, placed in order so later areas never move earlier copies, `scatter.paint` strokes and erases as one undo step, the Art palette's Scatter paint Scene tool, and `entity.get` copies and status | implemented |
| [103](adr/103-probe-and-volume-authoring.md) | Probe and volume authoring: edits keep runtime-only component fields, probes keep their authored enabled state, project saves rewrite the document's `reflection_probes` from probe entities, the overlay writes only records it can read back, `probe.create` and the Lighting palette's Probe, Bakery destinations for new probes and the `bake.scene.json` capture scene, harness children registering the FPS module's components, the diffuse volume box in Bake settings, outline drag handles and the Scene toolbar time chip | implemented |
| [104](adr/104-desktop-baked-lamps.md) | Baked static lamps on the desktop pipeline: lamp-group RGB9E5 or BC6H irradiance and RGBA8 or BC7 direction planes, removal of static lamps, G-buffer resolve images shaded as undirected and directed light, volume lamp bands for moving receivers, transmission sampling, moving-caster shadows of at most two baked lamps chosen by their light at the movers the camera sees, the Bistro night evidence, and the Mac evidence for the shared payload and the tiled bake | partial |
| [105](adr/105-network-transport-and-asset-depot.md) | Network transport, data format and asset depot: nonblocking dual-stack UDP sockets, the `vkr_net` sans-I/O core with the Noise IK handshake (X25519, ChaCha20-Poly1305, BLAKE2b), AES-256-GCM packets, five delivery classes on up to 64 channels, flow control, BBR-style congestion control with adaptive reordering thresholds, PMTU discovery and path validation; the UDP host and deterministic simulator; the bit stream, exact-schema wire codec and schema hash with no text on the wire; versioned services over sessions; the `vkr_depot` content-addressed store with FastCDC chunks, commits, compare-and-swap branches, path locks, access roles, working copies, the sync library, LAN discovery, server-to-server mirroring and its command; the host thread with sub-millisecond waits, ChaCha20 header protection, pacing and background-mode fixes on a shared simulator bottleneck; access requests granted at runtime, the server service thread, the discovery browser, clone, status, log, progress, cancel and conflict policies, the identity key in the user data folder; fuzzers under ASan and UBSan; `vkr_net_bench`; Windows loopback and simulator evidence; Mac M1 transport, depot and fuzz evidence and Mac–Windows depot round trips; cross-machine throughput, Windows runs of the 2026-10-12 changes and an independent review pending | partial |
| [106](adr/106-collaborative-editing-session.md) | Collaborative editing session: the editor's `collab` service, host and join with a scene digest and history replay, edits by document id through type descriptors, host-ordered injection into the request slots with journal verification so every journal mirrors the host's, shared undo, group mapping for reverts, gizmo drags reported by the runtime and applied ahead on participants, presence data, refused edit kinds, the `session.*` operations; seeded duplicates; terrain ops applied everywhere as the host's samples, with new terrains staged on every editor; the Session window and other editors' cameras, names, selections and gizmo drags in the Scene; agent federation with `agent@editor` authors, the host's claims and task board (`task.*`, with capabilities and slots), a session-wide change feed and reviews across editors; physics, collision-layer and scene-settings edits; hosting from a depot commit that joiners check out, publishing files through the depot and newer depot bases for long histories; access by allowed identity keys; Scene pointer rays; the two-editor and depot Bistro checks; partition edits, automatic publishing and macOS or cross-machine runs pending | partial |
| [107](adr/107-stationary-lamps.md) | Stationary lamps, removed the day they shipped: runtime direct light over baked bounce with a four-channel shadow mask; the owner review's ownership rings, Epic pixelation and doubled `Tiled.Opaque` cost, and what a new design must avoid | declined |
| [108](adr/108-baked-lamp-direct-pages.md) | Baked lamp-direct pages: each lamp group's direct light on denser tiled pages (Bakery default 16 texels per metre) through a direct-only gather, soft lamp shadows from `source_radius` on both bakers, VKLM v5 lamp-direct planes and rectangles, skyline packing and turned brush charts (v6), bicubic sampling in the tiled forward pass, tiled moving-caster shadows of at most two selected baked lamps in the `CASTERS` and `CASTERS_ALL` variants; moving physics bodies and mover-driven brushes as moving casters left out of the bake; the Level Design Test rebake, captures and timing; Vulkan bake unrun | partial |
| [109](adr/109-reversed-camera-depth.md) | Reversed camera depth: near at 1 and far at 0 on every camera depth buffer, cleared to 0 with greater-than tests, shadow maps kept forward with split camera and shadow depth states, the reversed HZB, sky tests, ray unprojections, linearizations, MSAA resolve and occupied-depth feedback, a 20 km default far plane with orthographic framing kept at 250 m, and the Metal Bistro before/after evidence; Vulkan compiles only | partial |
| [110](adr/110-project-sharing-in-the-editor.md) | Project sharing in the editor: Share into a workspace of its own served by the editor while it runs, access asked by name and approved in place, Get from the project hub with discovery and direct probes, Submit, Get latest and conflict choices, textures cooked and re-cooked for the receiving computer, the top bar button, the Project sharing window, `depot.*` operations and the macOS Local Network usage string; Mac loopback evidence; Windows and a two-computer run pending | partial |
| [111](adr/111-editor-presenter.md) | Editor presenter: a second Metal queue that composites the newest finished Scene image with the UI at the display's rate, `Editor.Present` copies, presentation-only frames, the runtime's Scene pacing by measured GPU time and refresh interval, presenter-aware completion, captures, preview and Render Stop presenting per frame; Vulkan presents per frame | partial |
| [112](adr/112-asset-references-and-live-asset-swaps.md) | Asset references and live asset swaps: `asset:<scope>/<id>` scene values resolved through lowered `asset_refs` and the render assets' reference table, material and texture drops from Content, cooked standalone textures, project-scope rebuilds with change reports, rebuilds that swap materials, textures and meshes into the open scene without a reload, and automatic rebuilds of edited sources and edited or removed build files; Windows watches and Vulkan runs pending | partial |
| [113](adr/113-brdf-material-extensions.md) | BRDF material extensions: F82-tint Fresnel with a DFG C channel, a dual specular lobe, EON rough diffuse and the Callisto smooth terminator as opt-in scalar material values with identity defaults, on both pipeline classes and (EON and F82) in the lightmap bakers; Metal evidence, Vulkan native runs pending | partial |
| [114](adr/114-per-object-draw-distance.md) | Per-object draw distance: a `draw_distance` component and kind defaults (1 px objects, 2 px and 50 m shadows for scatter copies), a 20-bit cull word in the candidate state, camera distance and screen-size tests and a cascade shadow-distance test in the shared classify kernel so retained shadows stay valid, the View distance setting, the Scene chip, ring and `draw_distance.set`; Metal evidence, Vulkan native runs pending, fade band not built | partial |
| [115](adr/115-rooms-and-portals.md) | Rooms and portals: `room` boxes around interiors and `portal` rectangles in openings, static draws' rooms per submesh (two entries of rooms and the outside, else unassigned), a per-frame portal walk with screen-rectangle narrowing, the hidden and sealed bits of the cull word for the camera and the cascades, light and reflection-probe gates, portals shut by door movers and `room.probes`; `blockout.room` and `blockout.doorway` create them, `room.derive` proposes them and the `rooms` lint check finds leaks; Metal evidence | implemented |
| [116](adr/116-scatter-proxy-cells.md) | Scatter proxy cells: past a scatter's `proxy_distance`, each 64 m cell of copies draws one merged mesh of their coarsest level, swapped by the camera's hidden bit with hysteresis while the copies keep casting shadows; Metal evidence, impostor cards not built | partial |
| [117](adr/117-desktop-planar-reflections.md) | Desktop planar reflections: the `planar_reflector` component, the frame's largest reflector above which the camera stands, its mirrored culling view shaded forward with every light and shadow into a half- or quarter-size image, and the reflector's on-plane pixels taking it in place of the probes; on the tiled pipeline, the reflector's draws bound to a baked plane-bound probe; tiled near field not built | partial |
| [118](adr/118-renderer-libc-boundary.md) | Renderer, runtime and editor libc and platform boundary: renderer, runtime, asset-reader and editor C targets compile behind `VKR_LIBC_BOUNDARY`, lib owners for math, text and number parsing, environment, files and directory listing, time, process output and exit, sorting, random bytes, file-manager commands and the C heap, background pipeline creation on the runtime job system with `vkr_renderer_detach_jobs`, the Win32 surface in the WSI unit, `runtime/src/platform/` as the runtime's window-system layer, C++ adapters on the lib wrappers, API-required GCD in the Metal backend, and the `check_libc_boundary.py` guard; Windows build unrun | implemented |
| [119](adr/119-dynamic-diffuse-volume-layer.md) | Dynamic diffuse volume layer: optional ray queries, a compacted BLAS per submesh and a refitted TLAS per frame slot from packed opaque rows, CPU selection of the light the bake lacks, the probe trace within a measured 1.0 ms budget, the signed layer added at lookup, the runtime grid for a volume not baked yet, a changed lamp's group leaving lamp lightmaps, and reflection probes relit by the layer; the editor does not write `runtime_grid` yet | partial |
| [120](adr/120-mover-lighting-origin.md) | Mover lighting origin: a mover's brushes and blockout shapes take baked diffuse light and their reflection probe at one point, the centre of what it moves plus the mover's `lighting_offset`, kept in each mesh's own space and published in a 160-byte prepared instance row, on both pipelines with per-pixel normals; mesh renderer instances unchanged; unbuilt and no native run | partial |
| [121](adr/121-tiled-temporal-upscaling.md) | Tiled temporal upscaling: the MetalFX temporal scaler as the tiled default from 0.6 of each axis with one jittered sample per pixel and an adaptive floor of 0.5, the `upscaler` setting with spatial 4× MSAA as choice and device fallback, camera, object and skinned motion, texture mip bias, capacity-sized inputs that keep the scaler across scale steps, the still-camera mean, and the M1 Pro evidence; no reactive mask or base M1 timing | partial |
| [122](adr/122-recordings.md) | Recordings: `.vkrrec` bundles with a checksummed chunk stream, field dictionary, index and crash recovery; one lock-free ring per producer thread and a utility-priority writer that never blocks frames; camera and frame records with simulation stopped; Watch playback with exact seeking as its own transport state; the editor Record control, REC indicator, playback bar and Recordings window with thumbnails; `rec.*` operations and the offline `vkr_rec` reader; no frame metrics, Play recording or authoritative overhead gate yet | partial |

## Proposals

These preserve unimplemented scope after removing shipped prerequisites and
retired API sketches. They are not scheduled commitments. Resolve their open
decisions before dependent implementation.

| Proposal | Scope |
|---|---|
| [Codebase audit remediation](proposals/codebase-audit-remediation.md) | Applied owner decisions and what remains after the 2026-09-23 audit: re-checking the Vulkan text baseline on a Vulkan machine, the Bistro re-cook, deferred items and host-unavailable checks. |
| [Renderer features and performance audit](proposals/renderer-features-perf/renderer-features-perf.md) | Normal/fog corrections, optional material storage, screen-space and post-processing costs, and remaining native acceptance. |
| [Conditional D3D12 backend evaluation](proposals/d3d12-backend-evaluation.md) | Conditions for considering a third backend. |
| [Dedicated transfer queue](proposals/dedicated-transfer-queue.md) | Independent upload submission and completion-safe publication. |
| [Deformable scene effects](proposals/deformable-scene-effects.md) | A bounded deformation pilot with shared pass and history inputs. |
| [Animation graph and baking extensions](proposals/compute-animation-and-editor.md) | Managed controller/sequence assets, fixed-step control, baking, GPU pose evaluation and preview extensions. |
| [Collision extensions and destructibles](proposals/entity-collision-and-physics.md) | Engine/UI research and remaining AVBD/destruction, deforming collision and active-ragdoll work; implemented contracts are in ADR-072. |
| [Code-first entity behavior and visual authoring](proposals/entity-behavior-system.md) | ECS gameplay with prefab composition/lifecycle, simulation-owned actions/events, weapons, character/camera behavior, visual authoring, native reload and performance acceptance. |
| [Script modules](proposals/script-modules.md) | Remaining script scope after ADR-079: named groups of a project library, exports, and scripts in additive scenes; carrying values across field renames, statically linked scripts for consoles and iOS, splitting editor tooling out of the game shell, and a TypeScript layer over generated bindings. |
| [Editor UI extensions](proposals/editor-ui-extensions.md) | Advanced widgets, accessibility, and floating-window ownership. |
| [Asset build system](proposals/asset-build-system.md) | Remaining `vkr_bakery` scope after ADR-077: the daemon, bundles and scripts on Windows and shader hot reload; managed-project bundles moved to Project build and packaging. |
| [Project build and packaging](proposals/project-packaging.md) | Remaining packaging scope after ADR-078: chunk reuse inside changed archives, per-scene archives, platform icons and version resources, notarization, a signed editor `.app` or installer, and the Windows/Vulkan package and distribution gates. |
| [Editor Projects](proposals/editor-projects.md) | Remaining Projects workflow, native-platform, inspection, retirement and frame-budget acceptance; implemented contracts are in ADR-069. |
| [Recordings](proposals/recordings.md) | Recording the camera, player, input, world changes and optional frame metrics under one recorder frame number: Watch playback from recorded state, Drive with recorded actions on the current build, exact same-build re-simulation with hash checks, and perf re-runs as matched benchmarks; capture tiers per backend and the frame cost contract; the Library, record overlay, player and performance view; `rec.*` agent operations, the offline `vkr_rec` reader and recording requests with a lifecycle; owner decisions, phases R0 to R6 and Bistro evidence. |
| [Portable path contract](proposals/portable-path-contract.md) | Remaining macOS, network-share and interactive scene-selection evidence gates; implemented contract is in ADR-070. |
| [Lighting efficiency](proposals/lighting-efficiency.md) | Desktop-pipeline (Vulkan) deferred lighting, local shadow and transmission costs: the Vulkan cost split, findings from the removed Metal implementation, and remaining ordered fixes: transmission compaction from layer 0, cheaper mask and transmission-depth data and half-precision BRDF terms; the static and dynamic shadow atlas layers shipped (ADR-019). |
| [Sparse diffuse volumes](proposals/sparse-diffuse-volumes.md) | Remaining sparse-volume work after ADR-054: light paths shared across layers, runtime ray-traced (DDGI-style) updates on the desktop pipeline, which replace the removed SSGI, and the tiled, café-facade and authoritative cost evidence. |
| [Graph-owned IBL baking](proposals/graph-owned-ibl-baking.md) | Declare queued bake resources and dependencies in the graph. |
| [Windows asset builds](proposals/windows-asset-builds.md) | Remaining Windows import work: the cold finalization's remaining CPU, the cook's dependency hashes and deferred cook, Defender and Dev Drive scan costs, ReFS clone evidence, a BC re-render difference, Windows check portability and ARM BC builds. Native BC, hard-linked and parallel workspace copies and x86 SHA-256 are in ADR-012 and ADR-077. |
| [Windows/Vulkan verification checklist](proposals/windows-vulkan-verification.md) | Active Windows/Vulkan record: native renderer subset executed; Vulkan feature output, cooker, HDR/DPI and manual editor gates remain. Vulkan is the desktop pipeline's only backend. |
| [Local shadow architecture](proposals/local-shadow-architecture.md) | Layered local shadows for the desktop pipeline without pop-in: a persistent shadow-map cache for static lights behind the existing visibility mask, the per-frame budget kept for moving lights, and contribution-driven priority; phases from indoor on RDNA2 and Ampere to open world and the RDNA3/4, Ada and Blackwell tier, with Bistro gates per architecture. The tiled pipeline shares only the atlas and face cache. |
| [Shadow quality](proposals/shadow-quality.md) | Jagged, popping and short sun shadows on both pipelines: the 2026-10-16 source audit's texel-density table and eight fit, cull and cache defects, hybrid sun shadows (owner decision 2026-10-16): cascades as the base and screen-space contact shadows on both pipelines, ray-query penumbrae near the camera on the desktop pipeline behind a measured cost gate with a sparse shadow-map clipmap as the fallback, and on the tiled pipeline shortened cascades with a cached far level, a sun-term correction that adds contact shadows after `Tiled.Opaque`, and SDF shadows behind a prototype gate; per-pipeline budgets. |
| [Meshlet cluster culling](proposals/meshlet-cluster-culling.md) | Measured geometry-bound Bistro cost on M1 Pro (removed Metal desktop implementation) and why finer culling has not paid: cook-time chunks cut local shadows 16% but added more per-draw cost elsewhere; cone culling buys 2–3%. Local shadow faces are vertex-bound and cause the fill bursts after a camera jump, so a compacted-cluster path scoped to local shadow views comes first. |
| [Static-scene batching](proposals/static-scene-batching.md) | Evaluate static geometry merging against current GPU draw preparation. |
| [Level design toolkit](proposals/level-design-toolkit.md) | Work after the toolkit's six implemented phases (ADR-084): face grid follow-ups (multi-rectangle patches, multi-corner moves, arches, pixel tolerances), glTF export, Replace with mesh, IO across containers, visual scripting, bent spline meshes, linked prefabs and stacked scatter rules. |
| [Artist toolkit](proposals/artist-toolkit.md) | The follow-up to the level toolkit, whose surface tags (ADR-084) and Standard material graphs with the Art workbench (ADR-093) are implemented: project tags, material functions, the Lookdev scene, layered Standard and budgeted Custom tiers; no shader compiles in a running game and no pipeline after a scene or cell is ready; tag-to-material themes for the art pass; look volumes, environment, light and bake tools; dressing; agent operations for each tool; the owner decisions of 2026-10-09, phases and Bistro evidence. |
| [World partition](proposals/world-partition.md) | Work after ADR-086: the streaming hitch budget, imported content and more containers in cells, script state across cell unloads, richer proxies, long views and project storage of cells. |
| [Level toolkit audit](proposals/level-toolkit-audit.md) | Review of the level toolkit, terrain, geometry LOD and world partition (ADR-084 to ADR-086): findings by severity with evidence, fixes made during the audit, and the verification each finding still needs. |
| [Level toolkit Windows/Vulkan handoff](proposals/level-toolkit-windows-vulkan-handoff.md) | Steps for a Windows Vulkan host to verify the level toolkit, terrain, geometry LOD and world partition natively, with the commands, expected results and the audit findings each step closes. |
| [Short-history rendering](proposals/short-history-rendering.md) | Owner decisions of 2026-10-12 and their work in order: removal of SSR, SSGI and FSR 3.1; the reflection probe audit with its fixed and open rows, tiled per-draw and room probes and planar water reflections; spatial-only GTAO; GT7 tone mapping replacing AgX on both pipelines; SMAA T2x replacing portable TAA and FXAA; DDGI-style diffuse volume updates on the desktop pipeline; open choices and the Bistro evidence for each step. |
| [Decal channels and receivers](proposals/decal-channels-and-receivers.md) | Next ADR-092 decal features for both pipelines: normal and surface (roughness, metallic) opacities, a `decal_receiver` component with static-on and skinned-off defaults, mesh decals through a decal visibility layer, and packaging component material paths; open choices and the evidence needed. |
| [Tiled graphics pipeline](proposals/tiled-pipeline.md) | Design of the ADR-087 tiled pipeline: the stage-by-stage comparison with the desktop pipeline, the prototype measurements that chose its structure, lightmap phases, and the remaining work in order: per-draw reflection probes, baked AO in the lightmap alpha, gather PCF for shadowed dynamic lights and thick glass; no SSR. |
| [Tiled temporal upscaling](proposals/tiled-temporal-upscaling.md) | Work ADR-121's MetalFX upscaling leaves open: a reactive mask for glass and blended surfaces, base M1 timing, the mip bias for Custom material graphs, the HZB history under jitter, anti-aliasing for frames outside MetalFX, the auto-exposure check, and risks for particles, lighting changes, scrolling textures, disocclusion trails and the framework's closed history. |
| [Network protocol](proposals/network-protocol.md) | Work after ADR-105 and ADR-106: asset streaming into GPU memory, the rest of editor collaboration (publishing on its own, partition edits) and of agent federation (capture differences between pipeline classes) with a task board, and view streaming; game networking moved to its own proposal on 2026-10-16; the gaps of the implemented phases (cross-machine numbers, the threaded host's editor evidence, sparse working copy, shared Bakery cache, an independent review, NAT traversal); owner decisions of 2026-10-09, phases, targets and the Bistro evidence for each phase. |
| [Game networking](proposals/game-networking.md) | Deferred on 2026-10-16 until after the network protocol phases: layered multiplayer modeled on Star Citizen's server meshing, with the failures CIG reported mapped to rules; prediction and interpolation on clients, a stateful replication layer with per-client acknowledged baselines, relevance, priority and rate tiers, entity authority with leased, abortable transfer between simulation servers, an overload policy, and persistence with transactions; one in-process configuration for 10 to 20 player FPS sessions and a mesh for persistent worlds of 10,000+ players with 200 to 400 in one place; the gameplay, physics and streaming prerequisites, phases G0 to G6, targets, open decisions and Bistro evidence. |
| [Planar reflections](proposals/planar-reflections.md) | Planar reflections on the tiled pipeline, after its baked plane probe (owner decisions 2026-10-12 and 2026-10-13; the desktop pipeline's are ADR-117): a mirrored, clipped, reduced-resolution render pass before `Tiled.Opaque` culled as one more GPU-driven view, open choices with recommendations and the Bistro evidence needed. |
| [Large-level rendering](proposals/large-level-rendering.md) | Four features for large levels with a wide outdoor view over an indoor base, measured on the Level Design Test scene: per-object draw distance tested per view in the classify kernel so retained shadows stay valid, scatter cell proxies of crossed cards and merged rocks past the shadow distance, water planes with a baked plane-bound probe and a later real-time near field (exact mirror reflections cannot be baked), and rooms with portals that gate draws, lights, shadows and per-room probes; options, recommendations, open questions, phases and the Bistro evidence for each. |

## Maintaining this tree

Follow the [documentation skill](../.codex/skills/vkr-docs/SKILL.md). Keep current
behavior in the architecture and owning ADR, terms in the glossary, and future
features in proposals. Runtime graph and harness parsers plus checked-in inputs
own their contracts; there are no separately maintained descriptive JSON schemas
under `docs/`.
