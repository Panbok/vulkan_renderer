---
status: partial
updated: 2026-10-14
authority: architecture
---

# Renderer architecture

VKR is a C11 renderer with Metal 4 on macOS and capability-gated Vulkan 1.4 on
Windows. Both consume explicit frame inputs and an authored render graph: Metal
runs the tiled graphics pipeline and Vulkan the desktop pipeline
([ADR-087](adr/087-gpu-class-graphics-pipelines.md)). Native
implementations own GPU resources, pipelines, commands and completion; shared
code owns portable contracts and scene-facing systems. Linux, D3D12 and the
retired Vulkan 1.2 renderer are not current execution paths.
Supported devices are Apple M1 and newer, AMD RDNA 2 and newer, and NVIDIA
Ampere and newer; [ADR-083](adr/083-supported-hardware-matrix.md) records their
capability differences and the memory floor.

This document describes code present on 2026-09-13. It does not certify a fresh
native run or a performance result. [INDEX](INDEX.md) locates accepted decisions
and proposals; [CONTEXT](CONTEXT.md) defines vocabulary.

## Library boundaries

The reusable libraries form an acyclic application boundary. `vkr_foundation`
provides common containers, memory, math and platform support from `lib/src/`.
`vkr_render_contracts` owns shared rendering values and CPU packing, tangent
and color-transfer helpers used by the renderer and offline tools. `renderer_lib` owns rendering and native backend work only: it
accepts frame data and an optional `VkrNativeSurface`, but has no window, input,
event, scene, loader or cooking owner. The renderer, the runtime and its
asset readers, and the editor reach the C library and the operating system
only through `vkr_foundation`: their C targets compile behind `VKR_LIBC_BOUNDARY`,
`runtime/src/platform/` alone calls window-system APIs, and
`tools/checks/check_libc_boundary.py` rejects direct calls
([ADR-118](adr/118-renderer-libc-boundary.md)).

`vkr_asset_formats` reads versioned cooked artifacts. It contains runtime
decoding, not source import or artifact encoding. `vkr_runtime` builds on the
renderer and format libraries. It supplies the reusable application host,
standard scene runtime, runtime core services, and scene-facing systems.
C script modules under `scripts/` include only `sdk/sdk.h` and the foundation
headers it reaches, staged as one include root that the editor distribution
ships as `sdk/` (`cmake/vkr_script_sdk.cmake`), and call the engine through the runtime script host's SDK table
([ADR-079](adr/079-c-script-modules.md)).
`vkr_sample_runtime` is an optional consumer that supplies sample control and
presentation policy for the app, the editor and the packaged-game player
(`vkr_player`, [ADR-078](adr/078-project-build-and-packaging.md)). `vkr_asset_cooking` is tool-only;
it owns source import and cooked-artifact encoding and is not a runtime
dependency. `vkr_physics` is an explicit C++17 Jolt adapter consumed by the C11
scene runtime. It exposes C value/handle operations and owns no renderer state;
[ADR-072](adr/072-entity-collision-and-rigid-body-physics.md) records its pinned
dependency and simulation boundary.

`vkr_net` is the network transport, its binary data format and sessions; it
depends only on the foundation (which owns UDP sockets) and the vendored
libsodium (`vkr_sodium`), so servers and tools link it without the renderer.
`vkr_depot_lib` and the `vkr_depot` command serve and sync the asset depot
over it, through Bakery's file-system helpers (`vkr_bakery_os`)
([ADR-105](adr/105-network-transport-and-asset-depot.md)). `vkr_runtime`
links `vkr_net` for scene edits on the wire (`runtime/src/net/`), which the
editor's collaborative session sends in the host's order
([ADR-106](adr/106-collaborative-editing-session.md)). The editor shares,
gets and syncs projects through the depot, running the server on a host
thread while it is open
([ADR-110](adr/110-project-sharing-in-the-editor.md)).

`assets/` contains asset data only. Shared reader code lives in
`runtime/src/assets/`, offline producers in `tools/assets/`, and renderer code
directly in `renderer/src/`.

Runtime image decoding of explicit `source=only` requests and dynamic
system-font rasterization remain available; cooked textures upload as stored
host-native blocks (ADR-012). Offline mesh optimization, font atlas/MSDF generation and
texture encoding belong to tools.

Custom clients may link `renderer_lib` directly and own their loop and frame
inputs, or use `vkr_runtime`'s host without using the standard scene runtime.
The standard scene runtime owns the conventional scene/assets/camera/UI path;
the host owns window, input, events, timing and lifecycle, and invokes explicit
caller-state callbacks. Renderer callbacks never depend on link-time
`application_*` overrides.

The split passes independent renderer/runtime builds and serial Metal draw and
native-resize validation. Bistro color/depth capture generation passes; its
profile retains two unavailable manual-exposure telemetry assertions. Native
Vulkan execution remains unrun. See ADR-004 for the evidence boundary.

## Build policy

[CMake build policy](../cmake/vkr_build_policy.cmake) applies optimization after
upstream target options. Vendored and fetched dependencies, including the
separate KTX reader and mesh codecs, compile with `NDEBUG` and `-O3` (`/O2` for
MSVC) in every configuration. VKR-owned Release and RelWithDebInfo targets use
`-O3` or `/O2` plus function/data sections and linker removal of unused code.
Release and RelWithDebInfo additionally enable interprocedural optimization when
CMake's compiler and linker check succeeds; `VKR_ENABLE_IPO=OFF` disables it.
CMake applies its IPO flags to C and C++ only, so the policy passes the same
ThinLTO flag to Objective-C sources, including the Metal backend. GCC and Clang
targets also compile with `-fno-math-errno`, Apple's default, because no VKR
code reads `errno` after a math call. These settings do not enable fast-math or
establish a frame-time improvement.

The same policy compiles every VKR-owned Clang or GCC target with `-Wall -Wextra
-Wshadow`. It omits unused parameters, omitted designated-initializer fields,
sign comparisons and the brace elision used by `(Vec2){x, y}` vector literals.
Dependency targets are `SYSTEM`, so their headers stay outside the policy.
`VKR_WARNINGS_AS_ERRORS` adds `-Werror`; the shell and Xcode wrappers enable it
on macOS, where every VKR target compiles. Windows builds keep warnings
non-fatal until their platform sources are verified, and MSVC keeps its Debug
`/W4` level. Disabled log statements still type-check their arguments in
unreachable code, a compiled-out `assert_log` keeps its expression as an
unevaluated operand, and the clang static analyzer reads `assert_log` as an
assumption. The CPU tester undefines `NDEBUG` in every configuration so its
assertions always run; `VKR_TEST_LIBRARIES_NDEBUG` reports whether the linked
libraries were compiled with `NDEBUG`.

`VKR_TARGET_ARCH` selects the instruction-set baseline for every C, C++ and
Objective-C target, including dependencies, except Jolt and its `vkr_physics`
adapter: Jolt enables AVX2 and FMA from the compiler's target macros, which would
change physics results, so they keep the compiler default until physics
determinism settles that choice. It defaults to `native` for
GCC/Clang (`-march=native`), which tunes binaries for the build host, and to
`AVX2` for MSVC (`/arch:AVX2`). Binaries meant for other machines need a
portable value such as `x86-64-v3` or `armv8.2-a`. The x86 vector-math path in
`lib/src/math/vkr_simd.h` needs FMA (`x86-64-v3`, or `/arch:AVX2` on MSVC);
lower x86 baselines, including AVX without FMA, compile its scalar fallback.

[Root CMake configuration](../CMakeLists.txt) maps imported Debug,
RelWithDebInfo and MinSizeRel dependencies to Release. Windows uses the Release
CRT and `_ITERATOR_DEBUG_LEVEL=0` across configurations to match those libraries;
the selected triplet determines static versus DLL CRT linkage. VKR Debug retains
its own assertions and symbols. Release compiles INFO and above for both
application and editor; the application keeps an ERROR threshold and the
editor captures INFO. `VKR_EDITOR_LOGGING` is an explicit shared build option
that also compiles DEBUG/TRACE, so selecting a different executable leaves library compile settings
unchanged. [Build wrappers](INDEX.md#build-and-run) reuse one dependency graph per
configuration and request only each consumer's target closure.

[Sanitizer policy](../cmake/vkr_sanitizers.cmake) selects one
`VKR_DEBUG_SANITIZER` profile. `default` preserves the cached
`VKR_ENABLE_DEBUG_SANITIZERS` and `VKR_ENABLE_SANITIZERS` switches: new
non-Windows Debug trees enable ASan/UBSan, while native Windows defaults to no
CPU sanitizers. The legacy `VKR_ENABLE_SANITIZERS=ON` applies ASan/UBSan across
configurations and must be disabled before choosing an explicit profile.

| Profile | Debug instrumentation |
|---|---|
| `address` | ASan + UBSan; also LSan on non-Windows when the combined runtime links |
| `thread` | TSan + UBSan |
| `memory` | MSan + UBSan, origin tracking and position-independent executable flags |
| `leak` | Standalone LSan + UBSan |
| `none` | No CPU sanitizers |

ASan, TSan and MSan cannot share one executable. Configuration checks both C and
C++ executable links with the selected sanitizer flags and rejects unavailable
runtimes. Native Windows rejects `thread`, `memory` and `leak`; `address` also
rejects the default static CRT and requires a DLL CRT with matching dependencies.
These profiles require Clang or GCC drivers; MSan specifically requires Clang
on Linux, FreeBSD or NetBSD. No Linux renderer support is added, and there is no
full-renderer MSan gate on the current Windows/macOS execution paths.

Dependencies remain optimized. `thread` and `memory` retain instrumentation in
in-tree vendor code so synchronization and memory shadow state cross library
calls. MSan additionally requires an instrumented C++ standard library and
external dependencies supplied through the toolchain; normal prebuilt libraries
do not provide valid coverage. Build and runtime instructions belong to
[the validation skill](../.codex/skills/vkr-validation/SKILL.md#cpu-sanitizers).
A successful configure or build does not establish sanitizer runtime coverage.

## Ownership and source map

| Owner | Responsibilities | Source |
|---|---|---|
| Application host | Window, input, event dispatch, timing, shutdown and caller-state callbacks | `runtime/src/application/vkr_application_host.h` |
| Standard scene runtime | Scene, camera, lighting, shadows, UI, picking, resize events, frame scratch and input construction | `runtime/src/application/vkr_standard_scene_runtime.h`, `runtime/src/renderer/systems/vkr_scene_frame.c` |
| Renderer | Acquired-frame lifecycle, targets, derived frame data, native operation selection and the optional render thread | `renderer/src/vkr_renderer.c`, `renderer/src/vkr_native_surface.h` |
| Selected implementation | Native resources/pipelines, graph realization, record/submit/cancel, targets | `renderer/src/metal/`, `vulkan/` |
| Shared graph | JSON realization, dependency order, culling, subresource barriers | `renderer/src/vkr_rg_json.c`, `vkr_rg_compile.c` |
| GPU lifetime cores | Ranges, submit values, generation slots, ABI, capture requests | `renderer/src/vkr_gpu_*`, `vkr_capture_ring.*` |
| Render assets | Geometry, textures, materials, meshes, animation banks, fonts, persistent world text, loaders and load scratch | `runtime/src/renderer/systems/vkr_render_assets.c`, `runtime/src/renderer/resources/loaders/` |
| Scene physics | Authored bodies/collider children, staged editor mutations, fixed ticks and evaluated pose publication | `runtime/src/renderer/systems/vkr_scene_physics.c` |
| Script SDK | `sdk.h`: the context, handles, hooks and authoring macros modules use; the private table behind its inline calls | `sdk/sdk.h` |
| Script host | Linked and shared-library modules, hot reload, script instances per attached container, ledgers that release acquisitions, tasks on worker threads, behavior bindings, temp memory and the SDK's engine adapters | `runtime/src/script/vkr_script_host.h` |
| Editor scripts | Project `Scripts/` packages built into one project library: Bakery builds, diagnostics, loads before the project's documents, rebuilds on save and file changes | `editor/src/editor_scripts.c` |
| Script editor | Code editor in a floating window or the Scripting workbench's Script view: tabs, C highlighting, completion, diagnostics, drawn by `vkr_ui_code_view` | `editor/src/editor_code.c` |
| FPS script module | Sample player, weapon, camera rig, action animation and training platform, called through the SDK | `scripts/fps/src/fps_module.c` |
| Brushes | Convex brush polygons, UVs, carve, extrude and merge; per-scene rebuild of brush meshes, materials and generated cell collision | `runtime/src/level/vkr_brush.c`, `runtime/src/renderer/systems/vkr_scene_brush.c` |
| Population | Spline curves and samples; spline mesh and scatter copies as runtime mesh instances rebuilt from their rules, painted scatter areas ([ADR-102](adr/102-scatter-painting.md)), the population bound and picking through the rule's entity; spline and scatter overlay lines | `runtime/src/level/vkr_spline.c`, `runtime/src/renderer/systems/vkr_scene_population.c`, `editor/src/editor_physics.c` |
| Terrain | Tiled heightfield samples, operations, the tile file with its overview and streamed tile loads; per-scene loading, the tile window around streaming sources, overview tiles, tile mesh rebuilds, height field collision and saving; the sculpt tool and Terrain window | `runtime/src/level/vkr_heightfield.c`, `runtime/src/renderer/systems/vkr_scene_terrain.c`, `editor/src/editor_terrain.c` |
| World partition | Cell assignment, the cell table and load plans; cell documents and the unload rules; per-frame streaming, Play holds, proxies and the origin rebase; the World Partition window and `partition.*` operations; `bake proxies` | `runtime/src/renderer/systems/vkr_scene_partition.c`, `runtime/src/renderer/systems/vkr_scene_edit.c`, `runtime/src/vkr_sample_runtime.c`, `editor/src/editor_partition.c`, `tools/bakery/vkr_bakery_bake.c` |
| Entity IO | Publication of connections, the session's sensor drain, trigger, relay, timer, counter and mover state, delivery order, limits and the `[io]` trace; the editor's Outputs, Inputs and Route sections and Scene lines | `runtime/src/script/vkr_io_router.c`, `editor/src/editor_io.c` |
| Level checks | Walkable-floor sampling against the player capsule, `level.lint`, reachability, face picking and the Level checks window | `editor/src/editor_level.c` |
| Network transport | UDP sockets, the sans-I/O core (handshake, header-protected packets, channels, recovery, congestion control), the UDP host and its thread, the simulator with shared bottlenecks, the bit stream, the wire codec, sessions and the fuzzers ([ADR-105](adr/105-network-transport-and-asset-depot.md)) | `lib/src/platform/vkr_udp_socket.h`, `net/src/`, `tests/fuzz/` |
| Asset depot | Depot objects, the content-addressed store, FastCDC chunking, working copies, the depot protocol, server with runtime access requests, server service thread, client, discovery browser, sync steps with progress, cancel and conflict policies, and the command ([ADR-105](adr/105-network-transport-and-asset-depot.md)) | `tools/depot/` |
| Project sharing | Share, Get, Submit and Get latest in the editor; access approval, the top bar button, the Project sharing window, the hub's network list and the `depot.*` operations; Bakery's `share_project` and `recook_textures` ([ADR-110](adr/110-project-sharing-in-the-editor.md), [ADR-077](adr/077-asset-build-system.md)) | `editor/src/editor_depot.c`, `tools/bakery/project/vkr_project_share.c`, `tools/bakery/project/vkr_project_recook.c` |
| Collaborative editing | Scene edits and typed values on the wire; the editor's `collab` service, edit ordering, replay and presence ([ADR-106](adr/106-collaborative-editing-session.md)) | `runtime/src/net/`, `editor/src/editor_session.c` |
| Surface tags | Brush faces' surface tags and marks, their fixed greybox looks per face orientation, the greybox view and the migration of retired dev materials; surface themes that bind tags to materials per container over the World's, and faces sized by a material's `world_size` ([ADR-094](adr/094-surface-themes-and-art-pass.md)) | `runtime/src/level/vkr_surface.c`, `runtime/src/renderer/systems/vkr_scene_brush.c` |
| Material graphs | `.mtg` graphs, Standard lowering to `.mt` definitions, layered Standard materials blended by vertex colour, mask texture, slope or height ([ADR-095](adr/095-layered-standard-materials.md)), graph instances expanded by the material loader and the lightmap baker; the editor's material documents, their journal, the node canvas, the Graph view and `material.*` operations ([ADR-093](adr/093-material-graphs-and-art-workbench.md)); Custom graphs generated to MSL, the project library `vkr_bakery materials` compiles, per-graph tiled pipelines and GPU buckets with a Standard fallback on Metal ([ADR-096](adr/096-custom-material-graphs.md)) | `runtime/src/assets/vkr_material_graph.c`, `runtime/src/assets/vkr_material_codegen.c`, `runtime/src/renderer/resources/loaders/material_loader.c`, `renderer/src/metal/internal/vkr_metal_packet_custom.inc`, `tools/bakery/vkr_bakery_materials.c`, `editor/src/editor_material.c`, `editor/src/editor_graph.c` |
| Look volumes | `look_volume` components gathered at world resolution and blended at the camera each frame into the post-process globals, height fog, sky light and the metering range ([ADR-097](adr/097-look-volumes.md)) | `runtime/src/renderer/systems/vkr_scene_look.c`, `runtime/src/application/vkr_standard_scene_runtime.c` |
| Environment panel | The environment parts of a container with their sources, the Environment window, `.environment` presets and the `env.*` operations ([ADR-098](adr/098-environment-panel-and-presets.md)) | `editor/src/editor_environment.c`, `editor/src/editor_ops.c` |
| Artist views | The tiled inspection variant's data views and the tonemap's exposure false colour, `view.capture` modes and `query.luminance` over the HDR capture ([ADR-099](adr/099-artist-views-and-luminance-queries.md)) | `renderer/src/shaders/metal/msl/world/tiled.metal`, `renderer/src/shaders/metal/msl/post/tonemap.metal`, `renderer/src/shaders/shared/editor_view.slangh`, `editor/src/editor_ops.c` |
| Lighting tools | Bake settings and their request members, the time scrubber and toolbar time chip, selection outlines and drag handles of lights and volumes, the diffuse volume box, probe creation, the Lights window and the `lighting.*` operations ([ADR-100](adr/100-lighting-tools.md), [ADR-103](adr/103-probe-and-volume-authoring.md)) | `editor/src/editor_lighting.c`, `editor/src/editor_physics.c`, `editor/src/editor_ops.c` |
| Decal placement | The decal orientation rule, the Decal Scene tool and its preview box, and `decal.place` ([ADR-101](adr/101-decal-placement.md)) | `editor/src/editor_viewport.c`, `editor/src/editor_physics.c`, `editor/src/editor_ops.c` |
| Agent channel | Editor socket, operation table, batches, review, captures; `vkr_mcp` adapter | `editor/src/editor_agent.c`, `editor/src/editor_ops.c`, `tools/agent/vkr_mcp.c` |
| Physics adapter | Jolt world/body lifetime, native contact response/joints, sweeps and bounded contact/sensor events behind C types | `runtime/src/physics/vkr_physics.cpp` |
| Production shaders | Shared math and native bindings/entry points | `renderer/src/shaders/` |
| Offline tools/harness | Asset cooking, cases, captures, comparisons and profiles | `tools/` |

The application and editor are independent targets over `vkr_runtime` and the
optional `vkr_sample_runtime`. The app owns its F6 debug overlay; the editor owns its
dock composition with task workbenches and the Views panel that switches the
Scene with the Model, Graph and Script views, the Model view rendering a
runtime-owned model stage scene in place of the level (ADR-089), startup `--scene-only` mode,
a transient maximized Scene (ADR-027)
and windowless `--headless` scripted runs (ADR-075), and an agent channel: a per-user socket of typed
operations that apply as journal groups with designer review, and the
`vkr_mcp` adapter that serves them to MCP clients (ADR-084). Neither
executable imports the
other's source. `core/vkr_subsystem_plan` resolves application boot dependencies;
the GPU renderer does not own that subsystem policy. The editor has tab stacking, layout persistence, keyboard focus and icon-only
independent simulation/render controls in a draggable Scene toolbar, alongside
load/unload and camera entry. A second draggable bar groups camera, rendering
mode and grid dropdowns, with overflow in narrow panes. The runtime applies typed
view requests; Perspective restores its saved editor camera, and Top/Left/Right/
Bottom use explicit orthographic bases, image-plane panning and span zoom.
Orthographic selection enables the numbered/lettered world grid. The editor
projects at most 96 grid lines, skips intermediate lines in dense views and
suppresses overlapping labels; [ADR-027](adr/027-immediate-mode-grid-ui.md) owns
its display contract. Orthographic frames use spatial reconstruction and bypass
perspective-only effects while preserving the user's perspective settings; see
[ADR-046](adr/046-editor-viewport-mapping-and-picking.md).
Scene focus routes Tab to camera capture; panel
focus routes it to widgets. The Outliner reads the World and loaded scenes
through a virtualized tree, and double-clicking a row frames its object and
opens its script; a search starting with `#` finds objects by their
[tags](adr/084-agent-channel-and-level-design-toolkit.md#tags). Details sends
typed selection and edit requests to the runtime. Debug > Labels controls object icons: lights, scripted objects and
other placed objects draw above their origins, including an empty-object icon
for placed objects that nothing else draws or marks, and abstract World objects
(sun, sky, fog, post process) stack above the world origin, projected with the
packet's unjittered camera and Scene mapping. Icons behind collision and
past the icon distance fade out, testing a bounded number of icons per frame
([ADR-027](adr/027-immediate-mode-grid-ui.md)). The selected object's icon
fades and lets clicks through to the gizmo beneath it. A quick right click in the
Scene opens the menu of the object under the pointer.
Details rows come from each component's type descriptor and apply through the
edit journal.
RMB holds free-camera capture; F, Tab/F3 and the toolbar remain toggle alternatives.
Console snapshots bounded structured logger history with a checkbox filter dropdown.
The Build menu packages the open project and its Build tab shows the stages;
Bakery sits under Develop. In
legacy scene mode, Bakery runs twelve recipes—mesh, font, single texture, texture directory, GGX DFG, Charlie,
anisotropy, diffuse volume, reflection probe, collision hull, collision mesh
and shaders—as one cancellable
[`vkr_bakery`](adr/077-asset-build-system.md) child process at a time. Cook,
table and shader recipes stream its JSON events into per-action progress and
Console lines. Its setup, jobs and output views use labeled controls and
adapt to dock width; cancellation terminates the complete child process tree.
The diffuse recipe defaults to a tracked enclosed-room example; invalid-volume
bounds and child-baker diagnostics are reported in Bakery output.

`vkr_bakery` is the only asset and shader build program: cooked assets, lookup
tables, the shader catalog, managed project jobs, scene bakes, material
previews and C script modules share one per-user action cache, scheduler and
event stream. Builds run
`vkr_bakery shaders` into `<build>/shaders`; the renderer resolves SPIR-V, MSL
and `.metallib` files through the [shader catalog](../renderer/src/vkr_shader_catalog.c),
and Metal loads a metallib only when its manifest matches the MSL source.
Each editor process supervises one `vkr_bakery serve` daemon (macOS; Windows
has no transport yet) that recompiles the loaded shader catalog when a shader
source changes, then asks for a restart because pipelines do not hot reload,
and marks Content items Changed or Missing when their files change on disk; an
edited source of an asset in the open scene queues its Rebuild job unless the
scene has unsaved edits. Relative asset paths resolve against one content root
(the repository, an installed program's `content/` directory, or a bundle's
`content/` directory), and mounted `.vkpak`
archives serve identities below it through the ordinary filesystem calls;
`vkr_bakery bundle` packs a scene's closure with the runtime and shader
catalog into a directory that runs without the repository. Given a managed
project, it lowers the World and the included scenes to content identities,
packs `game.vkpak` and `engine.vkpak`, stages the `vkr_player` template and
publishes the package only after verification; the editor's Build menu runs
it on Bakery's worker, and the player opens the World and startup scene
through the requests the editor issues
([ADR-078](adr/078-project-build-and-packaging.md)). `build_editor_dist.sh`
installs the editor, its companion programs, engine content and player
template as one relocatable folder that keeps writable state in per-user
directories (ADR-078).
[ADR-077](adr/077-asset-build-system.md) records the contract and its gaps.
Settings > Graphics has a left tab rail for Display, Quality, Lighting, Effects,
and Color and a clipped, scrollable right content area. The editor emits typed
`VkrGraphicsSettingsRequest` values; the sample runtime validates and owns their
application. HDR and texture resolution changes
show a restart-required notice; render scale and dynamic resolution do too unless the renderer's live
range covers them, and vsync only when the renderer refuses the present mode,
since it switches between frames (ADR-027). Other controls apply live and
invalidate the affected histories. Legacy app/scene settings load from
`VKR_GRAPHICS_SETTINGS_PATH` or `.vkr-graphics-settings.json`, debounce saves, and
flush on exit. Managed editor preferences use the project writer described below;
the managed editor starts from a machine-local `graphics.json` beside the
workspace locator, which keeps the restart-time Graphics settings (ADR-069). Render Stop retains the last Scene image while UI continues;
Vulkan UI-only frames reset Scene readback copies before skipping absent producers.
Scene allocation failures trigger bounded output-resolution reductions while UI
resolution stays unchanged; an error at the minimum stops Scene retries.
The first Metal GPU completion timeout flushes diagnostics and terminates the
process without GPU teardown, because completion is unproven;
see [ADR-046](adr/046-editor-viewport-mapping-and-picking.md).
Editor details are in [ADR-027](adr/027-immediate-mode-grid-ui.md).

The normal editor starts at a Projects chooser without a world scene. A selected
workspace directory contains `.vkreditor`, including UUID-named projects, managed
scene manifests, copied imports, versioned editor font bundles and generated
artifacts. The project writer owns durable editor preferences and per-scene
viewport/hierarchy recall. Periodic preference saves publish an owned snapshot
on one background worker; newer recall remains pending until the next save.
Explicit saves and project transitions drain that worker before publication or
workspace release, preserving durable writes and manifest conflict detection.
Authored environment, lights, probes and overrides
remain scene data. Explicit `--scene` retains legacy startup.
Creating a project selects Blank, FPS Arena or RPG Grounds, or No starter scene.
Starter scenes import Blender GLBs through the managed asset owner with an
explicitly authored static collision proxy and an empty Player Spawn entity.
Their daytime sky light and sun override the World. Their disabled cloud layer
does not resolve, so the World's clouds show while the scene inherits the World;
their scene journal owns collision and their `fps_player` camera mode starts
editor simulation in first person (Blank/FPS Arena) or third person (RPG Grounds).
No starter scene publishes only the project. Opening a project loads its World
(`world.scene.json`, created as a blank level of sun, sky atmosphere, clouds,
height fog and post process when missing) asynchronously, with a grid and the
camera above the origin; imported models can be placed at its root, in a new
scene or in a project scene ([ADR-076](adr/076-project-object-model.md)). Project
resource preparation does not mark a scene as loading, and an explicitly
selected starter scene opens after its creation job. Creation publishes the project manifest
before its first job, so a failed first scene leaves the project listed.
Successful write jobs remove unused cache entries, stale revisions, abandoned
directories and old job logs; ADR-069 defines what stays reachable.
Confirmed scene deletion atomically removes project membership and recall, then
uses a background job to erase scene-owned files. Active scenes unload before
deletion; project-shared assets remain. Incomplete file removal offers Retry.

[Projects](../editor/src/editor_projects.c) coordinates native selection, creation,
import, dirty-state prompts, scene activation and progress. Preparation progress
and errors stay inside Scene. While the runtime loads, applies the overlay
and rebuilds the opened scene's brushes, terrain and scatter copies, a card
over the unblurred Scene, an Outliner row and Content marks show the step and
elapsed time, and other editor panels remain usable (ADR-069). Brushes
rebuild within about 6 ms of each update, and a brush's first mesh attaches
once the update's rebuilds end
([ADR-084](adr/084-agent-channel-and-level-design-toolkit.md#brushes)). The runtime
reads and parses a project scene's edit overlay on a job worker and applies
it within 8 ms of each frame before the scene activates (ADR-076). Its
[job runner](../tools/bakery/project/vkr_project_main.c) (`vkr_bakery project`)
validates scene versions 3 to 5 and lowers inventory references to explicit
runtime paths, keeping only the host pipeline class's lightmap set. Imports preserve cooked geometry
identity, copy material/texture dependencies and publish fresh artifact revisions
before changing manifest references. Scene saves use immutable overlay revisions
and manifest fingerprint checks. Workspace writer leases cover asynchronous
writers through shutdown. In a project the runtime keeps the World resident
beside the primary scene and up to six additive scenes, all sharing one physics
world, and retains its GPU-completion-based resource retirement. A scene
switch keeps the closed primary scene's mesh assets, with their materials
and textures, loaded until the switch after it, so returning to that scene
rebuilds objects without loading assets (warm assets,
[ADR-089](adr/089-editor-workbenches.md)).
[ADR-076](adr/076-project-object-model.md) records this object model:
descriptor-generated Details, structural undo, singleton resolution with a
per-scene inherit-World setting, a per-scene texture limit
([ADR-012](adr/012-texture-compression-pipeline.md)), World-only physics and
animation settings, presets, Set primary and the Outliner with scenes nested
under the World.

The Outliner's Add entity form appends a model or directional, point, spot or
rectangle light to the loaded writable managed scene. Model sources use the
existing import/cooking job. Add publishes the scene and reloads it, preserving
existing entity indices and saved overlays, then selects the added entity.
Unsaved edits use Save/Discard/Cancel before the job starts. Creation is a
manifest publication rather than an undo-journal entry; later Details edits
use the normal journal. Failed additions return to the form for correction.

The dockable [Content browser](../editor/src/editor_content.c) refreshes managed
inventories and owns a bounded thumbnail cache. It is a virtual file system:
the project's `content.labels.json` holds a folder tree plus each item's folder
and tags, and files never move. A left folder tree with tags and
back/forward/up navigate folders; one toolbar row holds actions, navigation,
the search and sorting. Virtualized tiles or a sortable
Name/Type/Location/Tags list show subfolders, then items: assets, scenes,
presets, the World and one built-in item per creatable object. Dragging onto a
folder moves an item; dropping on the viewport opens a scene, adds an object,
places a mesh or assigns a material or texture to the object under the pointer
by its asset reference; OS files dropped on the window open the import window
for the folder under the pointer. Edited sources and edited or removed build
files rebuild their assets, which swap into the open scene in place
([ADR-112](adr/112-asset-references-and-live-asset-swaps.md)). The right Details panel contains selection and folder
actions; narrow docks hide side panels. This follows Epic's
[Content Browser organization](https://dev.epicgames.com/documentation/en-us/unreal-engine/content-browser-interface-in-unreal-engine)
without exposing generated build directories as content folders.
Texture previews run in a CPU tool; material spheres run through a separate
harness job. Meshes and fonts use vector icons. `View > Content browser` or the Cmd
line `panel content` adds the panel to older saved layouts; Ctrl+Space toggles it, and new layouts place it
beside Console. Native
macOS/Windows dialogs use UI-thread calls and owned UTF-8 result paths. Windows
filesystem, startup arguments, importer reads, settings and logs share UTF-8
and extended native path boundaries. Managed references use a shared raw grammar
and checked serializer, separate from glTF URI and OBJ filename interpretation.
[ADR-070](adr/070-portable-path-boundaries.md) records these boundaries and their
regression gates; the [path proposal](proposals/portable-path-contract.md) retains
unavailable native evidence checks. [ADR-069](adr/069-editor-projects-and-workspaces.md) records
ownership, publication decisions, selected native Metal evidence and a
headless Windows Vulkan run of the scene opening. Broader native
Windows/Vulkan evidence, long-session stability and frame-budget acceptance
remain separate gates.

Managed scene and settings documents remain bounded to 1 MiB and nesting depth
32; a version 4 scene's immutable inventory revision and the project manifest,
which carries the project asset inventory inline, hold their records within
16 MiB. The
[project store](../editor/src/editor_project_store.c) sizes temporary token storage
from input bytes rather than imposing a smaller asset-count ceiling. Settings
updates merge owned fields into prior objects, retaining unknown nested fields;
arrays remain whole values owned by their serializer. UTF-8 names persist
independently of font coverage. Editor UI text uses the cooked Inter Regular
atlas, which holds every Inter glyph in Basic Latin, Latin-1, Latin Extended-A/B,
Greek and Cyrillic; user-authored names render in that face. Inter SemiBold
headings, the Console's Ubuntu Mono and authored labels cover Latin-1. Other
characters retain the existing missing-glyph fallback. This is fixed atlas
coverage, not an on-demand Unicode glyph service or an IME implementation.

The renderer loads its class's build-copied render graph through an explicit
configured path, alongside its compiled shader resources. Managed harness
processes keep workspace scene paths separate from the installation's bootstrap
working directory. A focused Metal material-preview run produced and validated a
256×256 sphere PNG; this establishes that preview path, not Vulkan output.

Transform editing is available through Details and gizmos with axis, plane
and center handles in world or local space; scale handles stretch one axis of
the object. Gizmo
gestures use normalized displayed-image coordinates so internal resolution
changes preserve active edits and delayed releases. The application submits a
bounded geometry overlay to both backends after tonemapping;
matching handle-picking draws take priority over scene surfaces. Handles keep a
fixed displayed size and remain outside lighting and temporal history. Local TRS
editing preserves scale and does not introduce shear under nonuniform parents.

Native operations are ordinary typed C functions selected by the platform build.
`VkrRendererImpl` stores properties without an operations table or untyped state
pointer. Assets retain the separate `VkrAssetPublisher` contract. There is no
per-draw dispatch table, frontend pipeline registry or generic command RHI.
[ADR-025](adr/025-selected-renderer-implementation-strategy.md) and
[ADR-024](adr/024-shared-bindless-gpu-cores.md) define the boundary.

## Frame protocol

1. The application host pumps window, input and events. The standard scene
   runtime consumes its resize mailbox and calls
   `vkr_renderer_begin_frame(renderer, &config, &frame)` with explicit shadow
   dimensions. Acquisition proves slot reuse and supplies target dimensions,
   target generation and retained-shadow state.
2. The standard scene runtime pumps render assets with explicit
   submission/completion state,
   then extracts scene, UI and text draws and assembles
   `VkrFrameInput` with borrowed arrays in scratch. Text edits happen through
   their owner before rendering.
3. `vkr_renderer_submit_frame(&frame, &input, ...)` validates input and target
   extent, derives private frame data, realizes the graph and prepares every
   enabled pass before recording native work, submitting, capturing and presenting.
   With a render thread that work continues on the renderer's worker;
   `vkr_renderer_complete_frame()` waits for it and returns the frame's result.
   `vkr_renderer_render_frame()` does both.
4. Rendering consumes the acquired `VkrFrame`. Input-construction failure uses
   `vkr_renderer_cancel_frame(&frame)` to release acquired resources and
   recorded-but-unsubmitted work. Input rejection also cancels.
5. Completed submission results feed timing, readback, retirement and history.

The frame-loop thread runs the host loop and builds each frame. The render
thread (on by default; `inline_rendering` or `VKR_RENDER_THREAD=0` renders
inline) renders frame N while the frame-loop thread builds frame N+1. A steady-state frame is decoupled:
`vkr_renderer_submit_unacquired_frame()` lets the render thread acquire it,
and `VkrFrameHooks` resolve shadow reuse and other acquisition-dependent
input there, so the render thread owns the shadow system. Frames that resize
or change the Scene output are acquired on the frame-loop thread first. Asset
publications are recorded as ordered commands that the thread rendering the
next frame runs before preparing it; their completions return to the asset
systems, which let frames name a mesh, loaded texture or baked environment only
once its publication is confirmed. Every other renderer call waits for the
frame being rendered. See [ADR-082](adr/082-renderer-owned-render-thread.md).

On Metal the editor presents through a presenter, a second queue that
composites the newest finished Scene image with the UI. Scene frames render on
the frame queue at the GPU's rate and copy their Scene image to it
(`Editor.Present`); frames between them carry no Scene and only present
(`VkrEditorPassPayload.present_only`). The runtime chooses each frame from the
Scene's measured GPU time and the display interval, so the UI presents at the
display's rate. Captures, the animation preview and Render Stop present once per
frame; Vulkan always does. See [ADR-111](adr/111-editor-presenter.md).

`VkrFrame` identifies one acquisition and supplies resolved target facts. It must
not be copied or modified; its renderer must outlive it. Consumed or stale frame
contexts are rejected. Acquisition identity is separate from GPU completion.

Frame-input version 48 contains frame metadata, camera/lighting/settings and typed
world, shadow, sky, baked diffuse-volume, rectangle-light, analytic-fog and
froxel-fog, UI, editor, picking and debug payloads. Supplied world-text and UI
streams are authoritative. `vkr_frame_input_validate()` checks structural input.
Private `VkrPreparedFrame` holds derived temporal, exposure, bloom, GTAO,
sky, analytic-fog and froxel-fog values alongside the borrowed input; those derived
fields and text mutations are absent from the public frame input.

Arrays remain caller-owned until the frame completes; the runtime keeps them in
frame storage and, with a render thread, copies update-mutable skinning, world
text and capture data into frame scratch. Retained assets use generation
identities and completion-protected storage. Acquisition precedes input validation;
rejection or recording failure must resolve acquired native resources. Residency,
retained graph contents and histories commit only after successful submission.
Earlier scene/text edits and resource publication are not a general transaction.
Frame inputs are not standalone replay recordings. See
[ADR-004](adr/004-stateless-render-packet.md).

## Scene extraction and publication

The standalone animation cooker produces `.vka` rig/clip banks. Shared asset code
validates and samples these banks on the CPU, including hierarchy and skin-palette
evaluation. Skinned mesh version 18 preserves four-influence data through cooking
and exposes it in the loader result arena; static version 17 remains supported.
Optional scene animation bindings retain mesh and `.vka` resource requests and
advance per-wrapper CPU poses after authored transform propagation. Optional inline
controllers evaluate weighted blend graphs, 1D/2D blend spaces and conditional state
transitions before hierarchy/palette publication. Clip crossfades and graph mixtures
blend local TRS; the existing compute pass consumes the resulting palette. Evaluated
poses remain separate from authored ECS transforms. Both backends consume them
through per-instance compute skinning, read by either pipeline's shadow, scene
and picking draws and by the desktop pipeline's geometry reconstruction and
motion. Conservative joint influence boxes supply current bounds.
The movable Animation editor has an independent player, graph/sequence controls
and a graph-owned model preview. Static geometry remains shared. Skinned captures
on the tiled pipeline and on Vulkan remain pending; no performance claim is
established.
[ADR-071](adr/071-animation-bank-and-reference-pose.md) owns this partial integration
contract and the scene configuration.

The standard scene runtime owns `VkrRenderAssets` independently of `VkrRenderer`.
Assets own
geometry, texture, material, mesh and font systems, persistent world resources,
loader contexts and their arenas, pools and asynchronous allocators. Assets have
a 64 MiB owner arena and 32 MiB load scratch; the standard scene runtime has a
separate 32 MiB frame scratch. The renderer has no duplicate owner or scratch arena. Native graph
DMemory remains backend-owned, and the Metal backend allocator query returns that
actual graph allocator. The assets borrow
the native `VkrAssetPublisher`; its renderer must outlive them. Cameras, lights,
shadows, UI, picking, gizmos, skybox and the active scene belong to the standard
scene runtime.
UI receives fonts, scratch, window and target extents explicitly. Scene operations
receive assets; only picking readback needs a renderer operation.

The standard scene runtime joins resource workers and proves GPU idle before
releasing scene/asset resources. Scene unload drains GPU use before destruction and any
teardown publication afterward. Partial initialization uses the same owner order;
loader contexts and asynchronous storage survive until workers and queued payloads
are drained. Scene-created shapes use auto-released geometry and transfer
creator references to the mesh manager; default geometry keeps its existing
persistent lifetime. The renderer owns neither resource-loader registration nor application-host events.


`VkrWorld` owns archetype ECS state and queries. `VkrScene` adds hierarchy,
transforms, resource references, punctual and rectangle lights, environment/probes,
one scene-owned baked diffuse-volume binding, analytic fog, text, projected
decals and render IDs.
glTF nodes retain local matrices, names and source identities; source geometry is
shared across node instances, with decal variants where world-offset corrections
differ. Cooked mesh v17 and v18 retain the same source hierarchy. Source fingerprints
protect editor sidecar overrides against reimport conflicts. Details supports
TRS where the authored matrix is decomposable; sheared matrices remain exact and
read-only. The runtime owns selection and a bounded undo journal. UI borrows
scene data for its current build and never keeps ECS component pointers.
`vkr_scene_handle_sync()` mirrors render-facing changes into `VkrMeshManager`;
`vkr_scene_build_world_draws()` scans mesh/instance/submesh records through
explicit mesh, material, publication and view inputs rather than reading ECS
archetype arrays directly. This bridge duplicates some retained data.

`VkrSceneSimulation` embeds the shared clock and optional owning-thread C hooks
before animation/physics and after contact dispatch. Configured scenes tick even
without rigid bodies; callback failure requires reset before further gameplay.
ECS query/tick scopes reject structural mutation while rows are borrowed, and
scene update refreshes cached queries when new archetypes appear. The native
weapon primitive provides tick-based ammo, reload and independent firing locks
with reservation before consumption. Gameplay lives in C script modules
([ADR-079](adr/079-c-script-modules.md)). A module calls the engine only
through `sdk.h`, registers its component types, which Content's Script assets
attach to objects by drag or an entity's Details script slot, and runs on the
scene clock; with no scene open, the World plays. A session attaches the
played container and the root World: a container-scoped module runs one
instance on each, a World-scoped one runs once. Behaviors run per entity
carrying their component. Each instance and behavior records what it acquires
in a ledger the host releases when the entity leaves, the instance ends or
its container unloads; a spawn may also end with an owner entity or after a
simulated lifetime. Structural edits made in fixed updates queue and apply
right after the tick, and what scripts spawn is never saved. The host is the played scene's only simulation
callback client and input observer. The editor session starts on the first
run or step and ends at Reset; Backspace restarts it.

A project's `Scripts/` packages, or `--scripts <dir>` in scene mode, build
with Bakery into one project library that loads before the project's
documents. Library packages share code with the packages that depend on
them. Saving in the Script editor, or changing a source on disk, rebuilds the
library and hot reloads every module in it between frames, or none. Instances keep their data unless its shape or
version changed, and compiler diagnostics mark the editor's gutter. The FPS
module, linked into the app, editor and
player, uses ordered input, a Jolt character capsule and a first/third-person
or shoulder camera rig. The player is an `fps_player` entity, or a capsule
spawned at the resolved Player Start. Its body is its own animation, mesh or
shape, else the default mannequin, which `vkr_scene_spawn_model` instantiates
from the engine content ([ADR-080](adr/080-default-mannequin-character.md)).
`--gameplay` adds a training platform in Bistro, and an `fps_weapon` entity
follows the player's animated hand.
Transient evaluated world matrices turn the avatar and attach its weapon without
changing authored TRS; presentation propagation does not consume simulation debt.
Editor pause and saved recall preserve the editing camera. A bank with the
mannequin's clip names is posed by a speed-synchronized locomotion controller:
one footfall phase advanced by distance over the blended stride, direction and
gait blends, crouch and jump states, posed per presented frame at the
interpolated root's time through `animation_sample_blend`. In third person the
body turns towards its movement with acceleration. Other banks select named
locomotion, airborne, stance and weapon clips through an interruptible C
playback controller whose animation advances on the scene clock. Ctrl
requests a prebuilt crouch capsule and clearance-checked standing. Adjacent
same-tick mouse look samples coalesce without crossing discrete actions, and
simulation faults preserve specific diagnostics. Losing gameplay input focus
clears held/pending commands while the simulation clock continues. Pause, disabled
physics and faults stop elapsed-time admission; focus changes do not reset its
epoch. General prefab/action authoring,
projectile pools and character interaction proxies remain unimplemented.
[ADR-073](adr/073-native-gameplay-foundation.md) owns their
contracts and remaining integration boundaries.

Scene physics owns bodies at any supported hierarchy depth and direct collider
children independently of render visibility. Static, Kinematic and Dynamic bodies
form compounds from boxes, spheres, capsules and cooked convex hulls; cooked
triangle meshes are Static/Kinematic. Composed transforms require positive scale
and no shear. Sphere/capsule scale is uniform; other shapes allow nonuniform scale.
Dynamic poses remain in world space while Static/Kinematic bodies follow parents.
Jolt owns contact response, mass/inertia, joints, sleeping and supported solid CCD.

The shared scene simulation clock runs at 60 Hz with at most eight ticks per update and retained
catch-up debt. Sixty consecutive overloaded updates pause simulation. Animation
samples before each fixed tick; bone attachments use global asset-space node poses,
and ragdoll bodies publish solved poses into the existing CPU skin-palette path.
Rendering interpolates completed poses; pause/Step show the exact current pose.
Reset clears clocks/debt and seeks animation to zero. Authored local TRS survives
simulation and Save. Queries provide rays, sphere overlaps and fixed-orientation
shape sweeps. Solid BEGIN/PERSIST/END callbacks run outside solver locks after each
tick; mutation must be queued until dispatch returns. Sensors have a separate
begin/end stream. Bounded event exhaustion faults the world.

The editor stages body, ragdoll and collision-matrix changes transactionally.
Version 3 overlays retain source identities, collider/joint IDs, bone references,
cooked paths and scene-owned named layers/presets. Legacy versions 1/2 remain
readable. Bakery and the standalone collision cooker produce validated VKC1 files
from static glTF/GLB proxies. Managed scene import retains collision dependencies
and remaps source references. Session mutes, impulses and evaluated poses are not
saved. Debug lines use the UI stream with a 512-line cap; shader and native packet
layouts are unchanged. SDK/adapter allocations remain outside VKR allocator-tag
totals. [ADR-072](adr/072-entity-collision-and-rigid-body-physics.md) owns the limits,
asset lifetime, transform rules and future solver boundary.

Extraction counts static and total candidates before allocation, then fills
disjoint static/dynamic spans in one source traversal. Transmission and direct
ordinary-blend side streams retain source encounter order. Stable identities
and static/dynamic/publication generations
let backend slots preserve unchanged static rows. Conservative camera culling
and back-to-front sorting apply to ordinary blend; opaque/cutout/transmission
and shadow classification run on the GPU.

Direct-draw preparation resolves live generations and submesh ranges before
native encoding. Vulkan omits pending geometry/material publication and preserves
ready draw order; invalid or stale references still fail frame preparation. Metal
has no pending native handle and rejects absent/stale references. Both encode prepared
rows. Each Metal geometry owns an exact-count CPU submesh array in the backend's
freeable allocator; publication copies loader ranges, and geometry destruction
reclaims them. Prepared GPU records contain values, so these CPU arrays do not
extend GPU retirement. Every native pass family resolves resources, roots and dispatch parameters
before command emission; picking and blend roots remain disjoint. Prepared
draw/dispatch recorders return `void`. Native object/encoder creation, command-buffer
begin/end, acquisition, submission and completion retain their failure boundaries. See [ADR-004](adr/004-stateless-render-packet.md).

Workers perform CPU-only resource preparation. Finalization on the frame-loop
thread records GPU publication; its upload budgets allow an oversized first upload to progress.
Required dependency/publication failure prevents scene activation.
Materials initially publish semantic defaults and request textures incrementally;
ready textures replace material rows. `vkr_material_system_replace` swaps a live
material's whole definition, streaming its textures as a group and publishing
them with the new factors in one row, which the editor uses to show a background
finalization's materials as they finish ([ADR-077](adr/077-asset-build-system.md)). Shared texture residency counts each GPU
texture once and completion-retires resources only after their last resident
reference leaves. Loader/cooker decisions are in
[ADR-030](adr/030-offline-mesh-optimization-and-cooking.md) and
[ADR-017](adr/017-prepared-specular-glossiness-lowering.md).

UI uses an immediate-mode API over a retained cache, grid layout and one bounded
indexed/scissored stream. Text/font systems own glyph layout resources and
cooked VKFA font loading; canonical derivative MTSDF coverage and DPI-derived
pre-layout sizing are shared contracts. Immediate labels and buttons use common
font line metrics and align their baseline to physical pixels after DPI resolution.
Text boxes use measured content, padding and borders; explicit per-axis minima
can enlarge them. Stretch tracks alone do not expand text leaves, while panels
and scroll containers retain their fill behavior. Wheel input accumulates per
frame and clears after its consumers run.
Icon polygons carry a one-physical-pixel alpha fringe in the shared UI stream.
The regular/bold bootstrap atlases use a 16-texel distance range at 64 texels/em,
providing at least a two-pixel reconstruction range for text at 8 physical pixels/em.
MTSDF atlas sampling stays linear when
scene-texture filtering changes. The app debug overlay has 11/13-device-pixel
minimum title/body sizes; its authored 9/11-point sizing still governs at higher
content scales. Its panels size to measured text, row gaps, padding and borders;
the performance block wraps within the panel's maximum width before measurement.
Both performance widgets share runtime-cached CPU/GPU identity
and process-resident/managed-GPU memory sampled once per second, alongside
render/output extents. See [ADR-027](adr/027-immediate-mode-grid-ui.md)
and [ADR-034](adr/034-offline-cooked-font-artifacts.md) through
[ADR-036](adr/036-dpi-derived-ui-text-scale.md). Resource worker/finalize ownership
is in [ADR-045](adr/045-resource-prepare-and-render-thread-finalize.md); stable
queued event payloads and latest-value resize delivery are in
[ADR-047](adr/047-event-payload-and-resize-mailbox-lifetimes.md).

## Graph and native execution

Each backend parses its class's authored graph: Vulkan
[`main.rendergraph.json`](../assets/render_graphs/main.rendergraph.json) for the
desktop pipeline and Metal
[`tiled.rendergraph.json`](../assets/render_graphs/tiled.rendergraph.json) for
the tiled pipeline. Both resolve conditions, aliases, names and repeats per
submitted frame and use the shared compiler for dependencies, ordering, culling
and barriers.
`vkr_render_graph_prepare_frame()` derives portable conditions, shadow counts,
HZB/transmission/bloom/GTAO mip counts and GTAO constants once from the prepared
frame. Native formats, resource instances and history/completion selection remain
with each backend. Both native registries load one executor catalog from
`vkr_render_graph_frame.c`, so every authored operation resolves to the
same executor id and pass type on either backend. Disabled declarations do not block startup. Both
classes draw the world from the same GPU-driven culling; no CPU-recorded
forward world path remains.

Shared native pass and timing storage covers the largest supported graph
expansion. The desktop graph's no-TAA path can expand beyond its temporal path
because it restores culling HZB generation. The graph-expansion test checks the
full supported repeat envelope before native emission;
[ADR-025](adr/025-selected-renderer-implementation-strategy.md) records the
ownership and bound. Metal records each run of consecutive compute
and transfer passes in one encoder, with intra-encoder barriers for the graph's
dependencies inside the run.

The graph describes image reads/writes/attachments, buffer access, compute
and indirect dispatches, and transfer uses. Image state is tracked per mip/layer;
compatible same-pass accesses combine before the barrier, and incompatible
layouts fail compilation. Same-layout writes still generate hazards. Buffer
barriers cover whole buffers. Compute/transfer pass types use the graphics
submission path and do not implement asynchronous queues or ownership transfer.

Metal emits producer dependencies by traversing the compiler-owned outgoing
edges; each consumer retains its declared resource barriers. Native backend
caches reuse graph resources until their resolved descriptions change.
Vulkan replacement waits for the greatest submitted use of the changed resource's
instances before freeing their views, descriptors and storage. Editor Scene-panel
resizes use this boundary independently of swapchain recreation.
`TRANSIENT` contents are frame-local, backed by reusable overlap-safe allocations;
transient aliasing is absent.
History owners select completed compatible instances. `RETAINED` image contents
have per-instance, per-subresource validity, seeded from submitted terminal state
and committed only after successful submit. Invalid retained reads fail graph
compilation. Cached allocation and `PERSISTENT` are not content proofs. Image
descriptors carry a separate depth dimension for 3D resources; 3D images require
one layer, one sample and no attachment use. Their history instances retain local
values that a later camera can reproject, while current-camera integrations remain
transient.

`IBL.Bake` is an authored uncullable compute pass, but its nested resource
accesses and barriers remain backend-owned. Uploads and portions of
capture/presentation likewise remain outside complete graph resource declarations,
with explicit native barriers and completion ownership. See [ADR-002](adr/002-render-graph.md) and
[ADR-029](adr/029-retained-graph-resources.md).

## Rendering pipeline

The graphics pipeline class follows the backend
([ADR-087](adr/087-gpu-class-graphics-pipelines.md)): Metal runs the tiled
pipeline on Apple M-series GPUs, including in the editor, and Vulkan runs the
desktop pipeline on immediate-mode discrete GPUs. No setting selects it. The
classes share GPU culling, the cascades and local shadow atlas, the sky,
clouds, IBL, exposure, bloom, tonemapping, editor, UI and text passes, the
material model and the kernels in `renderer/src/shaders/shared/`.

The tiled pipeline runs `tiled.rendergraph.json`. Its `Tiled.Opaque` pass
shades the culled opaque draws forward in one four-sample render pass
resolved on chip, with baked lightmaps for static
diffuse light; a scene without a lightmap set draws its static lights as
dynamic lights. A lamp group's direct light may come from denser
lamp-direct pages, sampled bicubically, and moving casters shadow the two
baked lamps that light them most ([ADR-108](adr/108-baked-lamp-direct-pages.md)).
`Tiled.Atmosphere` lays the clouds, aerial perspective and
analytic height fog over the resolved image, splitting pixels that mix sky
and surface samples between their media, and `Tiled.Blend` draws glass
with the blended surfaces, back to front; glass volumes refract and rough
glass blurs through a half-resolution copy of the image, taken only on frames
with either in view. Projected decals lay their materials' base colour over
the opaque surfaces in their boxes before lighting, in shading variants that
frames without a decal in view do not take
([ADR-092](adr/092-projected-decals.md)); the desktop pipeline's G-buffer
resolve applies the same decals. It
draws the 16 dynamic lights nearest the camera, 4 of them shadowed with a
four-comparison tent each, holding the set with an incumbent bonus and fading
lights in and out over 0.25 s, and no FXAA, screen-space effects or temporal
history. Its
adaptive quality lowers the internal render scale to as little as 0.65 while
frames miss a 16 ms GPU budget and upscales spatially
([ADR-039](adr/039-metal-internal-render-scale.md)). ADR-087 owns its passes,
evidence and budget. The rest of this section describes the desktop
pipeline, which only Vulkan runs.

Editor inspection adds Detail lighting, Lighting only and visible-edge Wireframe
to Lit and Unlit. The two lighting views use neutral material response; Detail
lighting retains mapped normals and Lighting only uses interpolated vertex normals.
Both pipelines draw these modes; their shaders consume shared inspection
helpers through the existing render-mode field. Their exact material, edge and
evidence contracts are in
[ADR-044](adr/044-shader-cross-backend-contract.md#editor-inspection-views).

The active graph conditions select direct or editor presentation, optional picking,
post controls and the temporal consumer. The main dataflow is:

1. Publish transforms/candidate tables, classify camera, cascade and local-shadow
   views (the camera also by per-object draw distance and screen size, rooms
   the portals do not show and far scatter cells, each cascade by shadow
   distance and sealed rooms; [ADR-114](adr/114-per-object-draw-distance.md),
   [ADR-115](adr/115-rooms-and-portals.md),
   [ADR-116](adr/116-scatter-proxy-cells.md)), compact visible rows, and
   encode indirect-count commands. Local
   shadows raster opaque depth and, when refractive casters exist, two RGB
   transmission crossings plus a blocking overflow depth.
2. Raster opaque/cutout visibility and depth, build HZB, and peel four ordered
   transmission visibility layers.
3. Resolve the G-buffer, which for lightmapped draws also writes the baked
   static lamps' irradiance and dominant direction when the scene's set holds
   them ([ADR-104](adr/104-desktop-baked-lamps.md)); evaluate GTAO; sample a
   bound sparse diffuse volume at half resolution, adding its ray-traced
   dynamic layer after `Volume.Build` and `Volume.Trace` update it
   ([ADR-119](adr/119-dynamic-diffuse-volume-layer.md)), upsample it with an
   exact fallback for unmatched pixels ([ADR-054](adr/054-baked-diffuse-volumes.md)),
   and compute HDR lighting. Optional profiled surface diffusion gathers the
   diffuse source. The graph then injects/reprojects local froxel scattering, integrates the current-camera
   froxel volume and applies it, then applies analytic fog before the opaque
   transmission pyramid. Transmission and ordinary blend sample the current
   integrated volume when froxel fog is enabled. Shade transmission from deepest
   to nearest, resolve requested picking, then draw ordinary blend.
4. Compose the jittered Scene HDR and SMAA T2x's history accept mask.
5. Meter exposure, produce/combine bloom, tonemap, run SMAA T2x on the
   display-linear image and compose native UI.

Optional profiled surface diffusion uses eight scene-authored RGB distance
profiles, 32 samples and a 32 internal-pixel radius cap. Two graph-owned RGBA16F
images hold the diffuse source and composite; the existing texture system owns
the immutable 8,320-byte profile bank. The offline baker samples the matching
full-tail surface BSSRDF, including direct and photon irradiance. A bounded
Windows Vulkan synchronization-validation resize passes. Vulkan withholds the subsurface graph until profile-bank
initialization completes, then fills the gather frame root at the dispatch owner before
material access. [ADR-068](adr/068-profiled-surface-diffusion.md) owns the energy
allocation, geometry approximation and evidence limits.

Optional motion blur runs after reconstruction and exposure metering, before
depth of field and bloom. The separate composites preserve temporal history.
[ADR-067](adr/067-post-reconstruction-motion-blur.md) and
[ADR-066](adr/066-post-reconstruction-depth-of-field.md) record their
contracts, budgets and native Vulkan evidence limits.

Shadow passes produce directional cascades when their retained reuse proof fails.
Source topology and submission policy are in
[ADR-028](adr/028-gpu-driven-deferred-visibility-buffer.md).

Static geometry uses 32-byte packed vertices and a 32-byte range decode record,
with float32 UVs. Cooking and publication validate range quantization; there is
no selectable 24-byte float16-UV mode. See
[ADR-031](adr/031-versioned-packed-static-geometry-abi.md).

Source instances remain 80 bytes; native publication/upload prepares 128-byte
instances with inverse-transpose normal directions and mirror handedness.
Tangents retain model-linear transport. glTF geometry remains local and node
matrices apply through the same instance contract. Cooked mesh versions 17 and 18
retain source hierarchy metadata and reject older flattened artifacts. Version 18
also carries CPU skin influences; the published geometry remains static. See
[ADR-044](adr/044-shader-cross-backend-contract.md) and
[ADR-030](adr/030-offline-mesh-optimization-and-cooking.md).

Camera depth is reversed (near 1, far 0, cleared to 0) and its depth-writing
passes use strict greater-than tests; blend/text depth reads accept equality.
Shadow maps keep forward depth and less-than tests
([ADR-109](adr/109-reversed-camera-depth.md)). Both cull with conservative affine sphere scale and retain odd
source edges during HZB reduction. Transmission compaction uses native subgroup
identity instead of assuming a workgroup invocation mapping.
Eight indirect buckets include reflection parity; on Vulkan each culling
view owns one row per candidate and each bucket an exact command slice sized
by the frame's packed candidates (ADR-028); direct blend preparation
splits contiguous parity runs without changing instance order. HZB metadata
separates camera compatibility from its exact raster producer grid. Jittered
frames skip history construction because the accepted conservative policy cannot
reuse another sampling grid. Optional emissive/debug resolve images are realized
only for requested captures, with specialized no-write production variants.

PBR materials use prepared metallic-roughness data with retained dielectric
response from specular-glossiness conversion. Texture color/data interpretation
is resolved during loading. Transmission preserves reflection/emission, replaces
diffuse, handles texture-driven thickness/attenuation and samples an ordered
background or an immutable opaque roughness pyramid. Four layers are the bounded
production policy; a fifth layer is diagnostic. See
[ADR-018](adr/018-graph-declared-transmission-feedback.md).

Clearcoat adds independent linear factor/roughness/normal maps,
a layered GGX response and one graph-owned RGBA8 G-buffer image per target
image. The coat and base lobes both reflect the global environment and
reflection probes. [ADR-062](adr/062-layered-clearcoat.md) records
the accepted scope and native evidence limits. Coated diffuse, metal and glass
share the directional energy allocation in runtime and the offline baker.

Charlie sheen uses a bounded layered response and two rectangle LTC integrals;
[ADR-063](adr/063-charlie-sheen.md) owns its 256 KiB shared table budget.
Anisotropic base GGX reflection retains authored tangent direction and adds
linear direction/strength maps, 6 MiB shared array tables and one RGBA8 axis/
strength G-buffer per target image. Runtime punctual/rectangle and offline
reflection share the material convention. Probe IBL keeps a scalar-filter
approximation; active anisotropy with refraction is rejected.
[ADR-064](adr/064-anisotropic-ggx-reflection.md) owns the encoding, input subset,
resource lifetime and measured fit limits.

Scene extraction aggregates opaque/cutout clearcoat, sheen and anisotropy flags.
The graph allocates each RGBA8 material plane only when its feature is present;
unknown external packet aggregates conservatively retain all planes. Native
consumers guard absent bindings and use zero-feature defaults. Previously
realized images may remain in the bounded graph cache until normal retirement;
disabling a feature does not imply immediate memory reclamation. Deferred
punctual and rectangle loops evaluate active lobes in one traversal, retaining
normal-specific coat visibility. Deferred lighting runs a base kernel without
the layer paths; when a plane exists, a layered kernel shades the 8x8 tiles
that contain layer data and the base kernel skips them. Both read local shadow
visibility from the `Shadow.LocalMask` pass, which runs only while a
shadowed light's range reaches the camera frustum or baked lamps' moving
casters show, and filters each such light's maps once per pixel into the
pixel's next overlap slot, in light
order, and multiplies in a short screen-space contact-shadow march; lights
past the eight slots are filtered inline. Valid baked-volume cells skip environment
diffuse evaluation and keep environment specular only in the share their
baked sky light supports, reflecting their baked light in the rest
([ADR-054](adr/054-baked-diffuse-volumes.md)); the tiled pipeline does the
same for lightmapped and volume-lit draws.

Thin-sheet diffuse transmission partitions residual base diffuse into front
reflection and tinted direct backlighting. Material-wide strength/color use
existing visible-draw/material tables in deferred lighting; there are no new
graph images. Opaque/cutout sheets exclude refraction and volume
thickness. The offline baker samples the opposite Lambert hemisphere without
changing media. [ADR-065](adr/065-thin-sheet-diffuse-transmission.md) owns the
runtime indirect-light approximation, shadow policy and evidence limits.

Offline texture mips use linear-light sRGB color filtering and area-weighted
footprints that retain odd source edges. Alpha and non-sRGB channels remain
linear. Repacking invalidates the former byte-box cooker identity. Explicit
cutout color jobs additionally use alpha-weighted RGB and per-mip alpha scaling
against the supplied material cutoff/factor. glTF MASK import generates and
references variants keyed by source content and material policy; equal recipes
share outputs. Materials with non-unit vertex alpha retain ordinary filtering.
Compatible glTF normal/MR inputs now receive paired recipe variants that retain
full normal moments and bake lost directional spread into GGX roughness. The
cooker folds normal strength/roughness factor into the images and publishes
both references together; factor-only materials gain an MR texture. Matching
extents, UV0 and sampler constraints, approximation limits and cache ownership
are recorded in [ADR-012](adr/012-texture-compression-pipeline.md).
See [ADR-012](adr/012-texture-compression-pipeline.md).

Punctual lighting uses a 128-light table, ranked by camera distance with
hysteresis and 0.25 s fades when more lights compete, and a 384-cell
fragment-local bitmask grid with exact range/cone rejection. Up to 16 ready
probes, listed smaller box first, take priority shares inside their boxes
before the global environment on the desktop pipeline; the tiled pipeline
gives a surface the first probe whose influence reaches one half. Directional lighting samples
CSM. Point/spot shadows use one 4096-squared D16 atlas array that all frames
in flight share, at most 8 (Balanced), 12 (High) or 16 (Ultra) layers, with
faces of 128 to 1024 texels, one per spot and six per point; presets draw at
most 12 (Balanced), 30 (High) or 60 (Ultra) faces per frame. Importance selection retains complete groups; excess
lights remain unshadowed. Vulkan ranks lights by the visible contribution
deferred lighting measured a few frames earlier, so enclosed and off-screen
lights take no faces; without a measurement, distance ranks. Static maps retain valid contents across frames, with
nine-tap PCF and point taps remapped across faces. A light's authored source
radius widens the mask pass's filter with blocker distance (contact-hardening,
ADR-019). Refractive casters use two
512² depth/RGB prefix layers and a blocking third-crossing depth. Per-tap
visibility combines opaque depth with receiver-gated RGB transmission, layered
reflection loss and material absorption. All five transmission images follow the
same complete-group reuse and submission proof as opaque depth. The
transmission arrays keep at most 32 faces' worth of 512² texels, halving the
crossing size for larger budgets; a full pool adds 672 MiB across three target
images and three raster passes per refreshed face;
straight rays do not model caustics. Directional glass-shadow behavior is unchanged.
Scene `casts_shadow` and the editor's Cast shadows checkbox require a finite
range. Imported glTF point/spot
lights default to casting shadows when their range is finite and positive and
their spot outer half-angle is below 90 degrees. Other imported local lights
remain unshadowed; saved editor overrides can disable shadows. Scene-authored
JSON lights remain opt-in. Light ranges,
probe bounds and GTAO do not establish arbitrary wall/furniture occlusion.
See [ADR-019](adr/019-bounded-forward-spatial-lighting.md).

Direct and environment lighting share height-correlated Smith GGX and an immutable
256×256 RGBA16F DFG. A per-surface energy record scales specular and reserves residual
energy for diffuse and transmission. [ADR-053](adr/053-energy-compensated-ggx.md)
owns the approximation and native evidence limits. Materials can opt into F82-tint
Fresnel, a dual specular lobe, EON rough diffuse and a smooth terminator; identity
values keep the base model on both pipeline classes and in the lightmap bakers.
[ADR-113](adr/113-brdf-material-extensions.md) owns their parameters, math and
approximations.

Scenes also support at most eight authored one-sided rectangular lights, including
disabled entries. The lighting system publishes a render-ID-sorted table; a frame
borrows it, and each backend packs 64-byte center/basis/radiance/color rows. Two
immutable 64×64 RGBA16F LTC tables occupy 64 KiB per renderer. Runtime LTC has no
area-shadow pass, while the offline baker traces rectangle visibility and transport.
[ADR-056](adr/056-rectangular-ltc-lights.md) owns authoring, lookup lifetime,
baker transport and native-evidence limits.

Scene-captured reflection probes persist as portable RGBA16F KTX2 cubes with
full mip chains.
A scene document's `reflection_probes` array holds them; in a project scene
the editor creates and edits probes as entities and rewrites that array on
save, and Bake lighting captures new ones
([ADR-103](adr/103-probe-and-volume-authoring.md)).
The offline baker captures six supersampled scene-linear views of the static
world and records source provenance; the cube carries its capture origin and
the diffuse volume's baked light there, by which the runtime scales the
probe's brightness with the time of day; box projection aims from that
origin, and a probe moved off it is flagged for recapture
([ADR-019](adr/019-bounded-forward-spatial-lighting.md)).
Normal scene loading uploads the cube and prepares SH/prefilter once. Ready local
probes work without a global environment. Local shadows are a persistent
cache: shadow-casting lights keep their faces resident in one 4096-squared
D16 depth atlas array that all frames in flight share, within the preset's
layer budget; while the lights fit it, face sizes follow light range and
layers the resident faces, and past it residency and sizes follow camera
distance with hysteresis while the layer count stays at the budget. Faces whose content is
invalid or stale redraw by importance within the preset's per-frame face
budget (High 30, Balanced 12, Ultra 60); a light's shadow fades in once its
faces are valid and does not switch off while they stay valid. Publications
mark the faces they may reach stale; a dynamic caster draws into a per-frame
copy of each face it reaches, and the copy is reused while the caster stands
still. A light that stops
casting fades its resident shadow out instead of dropping it. On the desktop
pipeline lights past the two most important take a single filtered tap and no
contact shadows; on the tiled pipeline every shadowed light takes the full
filter. The most
important lights also hold transmission layers for refractive casters.
[ADR-019](adr/019-bounded-forward-spatial-lighting.md) owns these policies.

The global environment source and GGX prefilter use cubemaps. Diffuse
lighting uses nine GPU-resident L2 coefficients for `E/pi`, with a black sentinel
and completion-safe replacement slots. IBL bake work is not fully graph-declared.
See [ADR-038](adr/038-sh-l2-diffuse-irradiance.md).

The scene atmosphere is the only sky. It bakes the global environment at an
authored observer altitude. Each generation owns its 136 KiB of RGBA16F
transmittance and multiple-scattering lookups; source, GGX prefilter, SH,
lookups and attenuated sunlight publish together after GPU completion. A
settings revision prepares a distinct candidate while the prior generation
remains active; a newer request waits for it to publish. Camera motion does not
rebake. A directional light flagged `atmosphere_sun`, the default in scene
files but not for glTF imports, drives the sun's direction, colour temperature,
irradiance and disc. The drawn sky, direct light and shadows follow it every
frame, with the direct light's attenuated irradiance integrated on the CPU;
the sky light re-bakes at most every 0.25 seconds while it moves. A
view-attenuated 1/theta^2 glow, `atmosphere.sun_glow`, makes the physical
disc read as a sun without changing lighting. A light flagged
`atmosphere_moon` is the atmosphere's second light, a phase-scaled moon that is
never the sun; the direct light, its shadows and the cloud shadow map follow
the moon while the sun is below the horizon
([ADR-081](adr/081-physical-night-sky.md)). A World `time_of_day` component
turns the sun and moon about the celestial pole by the hour, which advances
with the simulation, and fades its night light groups in after sunset; static
lights scale by their light group's factor
([ADR-090](adr/090-time-of-day.md)). Every frame builds a 384×108
sky-view image, a sun table beside a moon table, and a 32³ aerial-perspective
volume from the published lookups at the camera's altitude. The deferred
background samples both tables and adds an analytic limb-darkened sun disc, a
phase-shaded moon disc and, while the sun is below the horizon, a procedural
star field that turns with the sun about the celestial pole. Aerial
perspective attenuates surfaces before fog. Isolated fixtures and previews may instead author a
uniform constant source, uploaded as a one-texel-per-face cube and shown as a
uniform background. The scene `environment` block is the sky light: its enable
flag, intensities and SH window apply to either source, and a disabled sky
light keeps the atmosphere sky visible without IBL. Image skies and the
cross-scene fallback environment are removed.
[ADR-058](adr/058-revision-baked-sky-atmosphere.md) owns the model, numerical domain,
sky-light contract, publication and offline-baker integration status.

An atmosphere may carry one volumetric cloud layer, published with its
revision and drifted by a runtime wind offset. The renderer generates 2.1 MiB of
tiling R8 noise once. Each frame with clouds builds a 512² sun-projected
transmittance map and marches a half-resolution completion-gated history of
cloud radiance and transmittance, with aerial perspective at cloud depth.
Deferred lighting, or the tiled pipeline's atmosphere draw, composites it over
the sky, and every sun evaluation, including froxel injection and sky-lit
analytic fog, multiplies by the map. The revision bake remains a clear sky.
[ADR-074](adr/074-volumetric-cloud-layer.md) owns the layer and its measured
cost.

A scene may load one immutable baked diffuse-volume texture. The 8-by-probe-count
RGBA32F texture stores seven packed `E/pi` SH vectors and room/cell metadata; the
scene owns it and completion-retires replacement or reset. A validated cell proves
all eight trilinear probes share a room before their response replaces global/probe
diffuse indirect light. It never replaces specular IBL, and existing ambient
occlusion remains active. Invalid or uncovered cells keep the global/probe diffuse
fallback. [ADR-054](adr/054-baked-diffuse-volumes.md) owns offline room detection,
artifact layout, lifetime and evidence limits.

Screen-space reflections, screen-space diffuse indirect lighting and FSR 3.1
were removed from the desktop pipeline on 2026-10-12
([ADR-055](adr/055-screen-space-reflections.md),
[ADR-060](adr/060-screen-space-diffuse-indirect-lighting.md),
[ADR-052](adr/052-vulkan-fsr31-upscaling.md)). Specular indirect light comes
from the global environment and box-projected reflection probes on both
lobes of a coated material, and diffuse indirect light from lightmaps,
diffuse volumes and the environment. SMAA T2x replaced portable TAA and
FXAA on 2026-10-13
([ADR-037](adr/037-portable-same-resolution-temporal-antialiasing.md)).
On the desktop pipeline, a `planar_reflector` entity's surface on its plane
reflects the scene mirrored about it: one reflector a frame gets a culling
view and a forward-shaded image at half or quarter resolution, and its
pixels take that image in place of the probes where it covers them
([ADR-117](adr/117-desktop-planar-reflections.md)). On the tiled pipeline
a reflector's `probe` binds a baked plane probe that its draws use instead
(same ADR). With ray queries, a dynamic layer of the diffuse volume traces
the bounce the bake lacks, from dynamic lights, emissive movers and changed
static lights, within a 1.0 ms GPU budget. Where lamp lightmaps hold the
static lamps, a changed lamp takes its whole group out of the bake and the
layer lights it; baked reflection probes scale by the layer's light at their
capture origins. A scene whose volume is not baked yet gets a uniform
runtime grid that traces all of its light
([ADR-119](adr/119-dynamic-diffuse-volume-layer.md)).

Scenes may author analytic height fog. Frame preparation uploads one 48-byte
record per frame slot; a zero record bypasses fog. Fog in-scatters its authored
constant colour or, with sky lighting and a published atmosphere, the sun
through a Henyey-Greenstein lobe plus the sky light's average radiance. The in-place opaque/sky pass
runs after deferred lighting and froxel fog and before the opaque transmission pyramid, and also applies
atmospheric aerial perspective before fog. Transmission fogs only
new local lobes over already-fogged ordered feedback, and blend retains alpha.
Fog changes invalidate normal temporal content and request a scene-change
reset. The tiled pipeline
applies the same fog in `Tiled.Atmosphere` and its blend draws (ADR-087).
[ADR-057](adr/057-analytic-height-fog.md) owns the constants and composition;
native Vulkan fog output is not recorded.

Fog's 5,000 working radiance cap applies to fog accumulation, not final scene
composition. Empty media preserve finite scene HDR and nearly transparent media
attenuate it without a subsystem clamp. Froxel positions extrapolate a ray from
finite device-depth 1/0.5 points (the near plane under reversed depth,
[ADR-109](adr/109-reversed-camera-depth.md)) to the authored positive view depth, even beyond
the raster far plane. The independent production-Slang arithmetic regression
covers these changes; earlier native observations below predate the fixes.

Froxel volumetric fog is implemented under
[ADR-059](adr/059-froxel-volumetric-fog.md). The graph reserves frame-slot-count plus two
completion-gated RGBA16F 3D local-scattering histories and one transient
RGBA16F integrated volume per frame slot. The current two-slot renderer uses
six images (10.547 MiB at 1280×720), within the approved three-slot 14.063 MiB budget. Each enabled frame uploads a 944-byte parameter
record. Vulkan's frame root uses bytes 576, 584 and 588 for the parameter
address, integrated descriptor and sampler. The contract retains fields through byte 799 and appends unjittered
current view-projection and jittered inverse raster view-projection at bytes
800 and 864, and sky lighting at byte 928. Every light scatters through the
authored Henyey-Greenstein anisotropy; a sky-lit medium adds the sky light's
average radiance. Production Vulkan SPIR-V and host compilation checks pass;
native Vulkan froxel output is not recorded.

Directional shadows default to four 2048² cascades with snapping, fit hysteresis,
per-target-image reuse and shared PCF/bias units. The nearest two cascades add
eight-sample PCSS blocker search and at most sixteen filter samples, with an
authored 0.53-degree sun diameter by default. Farther cascades retain PCF,
or, with the **Filtered far shadows** setting that the High and Epic presets
turn on for the desktop pipeline, take one bilinear fetch of exponential
variance moments that a half-resolution pass rebuilds when the cascade redraws.
Static reuse requires guard
containment, matching generations, valid retained layers and a match with the
common submitted fit; stale physical copies redraw that fit once. A retained
cascade's culling view classifies but encodes no commands and skips its reset,
and casters smaller than one cascade texel skip the cascades. Incomplete
publication forces rendering. While dynamic casters exist, a reached or stale
cascade instead copies a shared static layer, drawn only when invalid, and
draws its dynamic casters over the copy. A composed cascade whose dynamic
casters have not changed since it was drawn is reused like static content.
SDSM and proactive refresh are opt-in;
fixed splits and zero proactive budget remain defaults. See
[ADR-041](adr/041-retained-cascaded-shadows.md) and
[ADR-033](adr/033-occupied-depth-sdsm-feedback.md).

SMAA T2x alternates two subpixel jitters and runs SMAA 1x on each frame's
display-linear image after the tonemap; its resolve blends the antialiased
subsample with the previous frame's, reprojected by rigid motion and weighted
by the reference's velocity term times a per-pixel accept mask. The
scene-linear `Temporal.Resolve` writes that mask from stable instance/submesh
identity and depth: a still pixel always accepts, a moving one needs its
surface among the 3x3 previous texels at a relative depth tolerance. No scene
colour history remains; history uses the preceding submitted frame with GPU
dependencies, output reuse still proves completion, and resets exclude every
pre-reset producer. Retained shadow images converge to a common submitted
projection so cached per-image fits cannot prevent convergence after camera
movement. The G-buffer writes sky rotation motion for the resolve
([ADR-037](adr/037-portable-same-resolution-temporal-antialiasing.md)).

Automatic exposure adapts over elapsed time since its selected completed state,
using a renderer-owned committed exposure clock and a bounded hitch policy.
Defaults lower exposure at 8 EV/s and raise it at 1 EV/s with a +24 EV upper target
limit, so physical night scenes are reachable. Exposure, bloom and GTAO have independent frame controls. GTAO's slice
basis and horizon signs follow view reconstruction; its noise is one fixed
spatial pattern that two edge-aware denoise passes remove, with no temporal
filter. RGBA8 outputs carry world
bent normals and visibility: global/probe diffuse uses bent sampling and
albedo-aware multi-bounce compensation, while baked volumes retain scalar AO.
A derived visibility cone occludes indirect specular.
Direct-light visibility remains shadow-owned. Direct lighting and the IBL PDF share the unclipped supported GGX lobe.
See [ADR-037](adr/037-portable-same-resolution-temporal-antialiasing.md)
and [ADR-042](adr/042-scene-linear-post-processing.md).

GT7 is the default display transform on both pipelines; ACES fitted remains selectable. Temperature,
tint, contrast and saturation apply after exposure and before the transform, leaving
metering and scene-linear history unchanged. Neutral grading bypasses its arithmetic.
[ADR-043](adr/043-presentation-dpi-and-color-transfer.md) owns the display contract.

## Presentation and platform boundaries

Optional EDR/scRGB presentation is implemented under
[ADR-061](adr/061-extended-linear-display-output.md). A window-owned display
snapshot feeds backend output selection and a 16-byte final/UI parameter
record. Metal output and transition checks pass; native Windows/Vulkan evidence
remains unavailable. Offscreen output stays SDR.

Windows uses Per-Monitor V2 physical client pixels. macOS renders the backing
scale unless the Graphics setting `high_dpi` is off, which renders one pixel
per point and can change without a restart. SDR final shaders emit linear
RGB into sRGB attachments; UI/text authored colors decode once before linear blending.
The frame's `image_sharpness` control is finite in `[0,1]`, with zero as an exact
bypass. The sample initializes it to 0.25; zero-initialized packet callers and
harness cases default to zero. A shared, neighborhood-limited sharpening filter
operates on tone-mapped linear Scene RGB in the existing presentation draw, after
SMAA on the desktop pipeline. UI, editor recomposition, diagnostic views and
temporal histories are excluded.
When SMAA or sharpening filters the final draw, an output-size RGBA16F
display-linear image is prepared first, so the display transform runs once per
pixel rather than per filter sample. SMAA runs on desktop frames in the default
render mode; the tiled pipeline draws no SMAA (ADR-087).
[ADR-043](adr/043-presentation-dpi-and-color-transfer.md) records its measured
cost.
`VKR_POST_TRANSFORM_CACHE=0` keeps the analytic per-sample reference path. The
final draw applies the physical output scale once; ADR-043 records the output
difference, cost observations and open native gates.
Internal Scene pixels, Scene presentation pixels and physical target/UI pixels
remain distinct. Picking and composition share viewport mapping.
See [ADR-043](adr/043-presentation-dpi-and-color-transfer.md).

Vulkan requires the explicit descriptor-buffer/device feature profile and rejects
unsupported devices; a Vulkan 1.4 version string alone is insufficient. There
is no legacy descriptor-set fallback. See
[ADR-023](adr/023-vulkan-1-4-bindless-capability-profile.md).
Requested material anisotropy uses the enabled device feature and effective
limit (up to 16); devices without it report a maximum of 1.
A Vulkan window created without a requested image count asks for the
renderer's present-target count, three by default, as its resizes do;
vsync selects FIFO and its absence IMMEDIATE, with a FIFO fallback
([ADR-014](adr/014-offscreen-present-target.md)).

Metal supports an explicit internal Scene scale, which the tiled pipeline's
adaptive quality steps between 0.65 and native while its tonemap pass upscales
spatially ([ADR-039](adr/039-metal-internal-render-scale.md), ADR-087 decision
12). Zero-initialized renderer API callers use unit scale, and the default
render scale is 1.0 on both backends. Neither backend has a temporal
upscaler. Vulkan renders the Scene at unit scale and rejects any other
scale; FSR 3.1 was removed on 2026-10-12
([ADR-052](adr/052-vulkan-fsr31-upscaling.md)).
Forward/deferred specular antialiasing shares the perceptual-to-GGX
roughness conversion on both backends; normal variance broadens squared GGX
width. Its measured benefit in the supplied Bistro view is small, and opaque
thin-edge motion remains a tuning gap. UI stays native after Scene
reconstruction. See [ADR-039](adr/039-metal-internal-render-scale.md).

## Memory, synchronization and observability

CPU storage uses arenas for bulk lifetimes, DMemory for individual release and
pools for fixed-size churn. Borrowed views can be invalidated by capacity growth;
allocator synchronization is explicit. Vulkan driver host allocations use null
callbacks and are outside VKR CPU totals. The renderer graph DMemory starts with
a 2 MiB commit and reserves 96 MiB of stable virtual address space. Vulkan uses
that owner for graph state, descriptor tables and per-block GPU allocation
metadata; the reserve covers texture publication while a resized graph remains
completion-protected. See
[ADR-006](adr/006-cpu-memory-allocators.md).

Vulkan descriptor/material publication requires coherent host-visible storage;
regular frame uploads retain their completion-protected fallback. A CPU geometry
mirror updates at publication/retirement/address changes. Each frame slot refreshes
its fixed upload-table prefix only when the mirror generation changes. The white
sampled/storage sentinel uses GENERAL with explicit first-use fragment/compute
dependencies. See [ADR-024](adr/024-shared-bindless-gpu-cores.md).

Container creation and growth report failure at their owning boundary. Failed
vector/hash growth preserves the previous contents and capacity; callers reserve
known batches before population and propagate allocation failure through existing
load, build or initialization errors. Partial application startup unwinds acquired
owners, and failed text rebuilding preserves its previously published layout.

Vulkan pools keyed device/upload/staging/readback blocks, with persistent mappings and
required dedicated-allocation exceptions. Completion-protected Vulkan frame slots
keep directly read uploads separate from copy-only candidate staging; both retain
capacity grown during frame preflight. Metal creates placement heaps on demand
and releases empty heaps after completed retirement. Its default managed
allocation cap, two thirds of the device's recommended working set, includes
heap capacity, upload/readback rings and explicit native buffers/ICBs; opaque
driver allocations remain outside that cap. Separate lifetime
groups keep asset textures from pinning retired Scene heaps. Transfer buffers grow
on demand after their GPU and CPU consumers finish. Graph draw tables use scene
candidate capacities on both backends; native caches retain sufficient backing
and wait for submitted users before replacing undersized buffers. Automatic texture pressure
accounts charged asset-heap capacity separately, and capacity retries require a
new finite high-water allowance or committed Scene reduction. Application owns
bounded Scene-output recovery for the paneled editor and standalone Metal app;
failed texture payloads are released, while their paths wait for a successfully
submitted reduction before retrying. Fullscreen Scene composition stretches to
the physical output and leaves UI native. App recovery stops at the existing
25% floor; standalone Vulkan has no Scene-output override capability.
Metal entrypoints use autorelease pools for temporary Objective-C objects;
resources that span calls retain explicit ownership and completion-gated release.
`VKR_METAL_MEMORY_BUDGET_MB` overrides the cap at startup. See
[ADR-024](adr/024-shared-bindless-gpu-cores.md) for budget and failure semantics. Its candidate preparation initializes only referenced
geometry rows in the existing completion-protected upload span; static residency
hits refresh those rows while marking resource use. Shared cores track logical
ranges, generations, submit values and retirement; physical allocations remain native. No VMA, online
defragmentation, GPU heap eviction or transient aliasing is implemented.

Slot and resource reuse require their actual last-submit completion. Vulkan
submission uses timeline values, while window presentation uses per-image
semaphores and either maintenance present fences or completed reacquire-wait
submissions. Metal maps completion to native command submission and event ordering.
Metal asset publication uses its own command slots and transfer ring, one slot
per ring slot, so an upload waits only for the upload one ring's length earlier,
never for a frame. Each publication run's geometry shares one upload batch; a
finished batch holding textures still returns only once they are on the GPU.
Frames keep at least two command slots of their own.
Lifecycle changes
may wait idle; ordinary successful frames do not wait the whole device.
Capture/picking results publish asynchronously and require release.
See [ADR-009](adr/009-frame-synchronization.md) and
[ADR-014](adr/014-offscreen-present-target.md).

Both backends publish completed GPU timing/results and allocation/visibility
metrics. Unsupported timing scopes are unavailable, never zero-duration proof.
Metal compute/graphics timestamps exist; a graphics interval spans its vertex,
tiling and fragment work, starting no earlier than the previous timed pass's
end. Transfer timing is not supported. Every Vulkan pass interval likewise
starts no earlier than the previous timed pass's end.
The harness owns case identity, artifacts, comparison and performance authority.
After resource/bootstrap readiness it starts authored warmup at the common zero
of the raster jitter and the cloud march noise period, with temporal history
invalidated; replay version 8 aligns to that 64-frame period.
See [ADR-015](adr/015-metrics-module.md) and [ADR-051](adr/051-renderer-harness-and-evidence.md).

## Remaining implementation and evidence boundaries

These are limits of current code or retained acceptance, not scheduled promises:

- Object model limits ([ADR-076](adr/076-project-object-model.md)): mesh
  Details are read-only, and physics attachments and joints keep hand-written
  Details rows. Prefab instances are unlinked copies. No Bistro case carries
  an animated mesh, so the animation component and World clock scale have
  CPU evidence only.
  The Windows `WM_DROPFILES` path has not been built or run. ASan/LSan
  evidence for unloading an additive Bistro scene is unavailable on this
  16 GiB host, and Apple ASan does not support leak detection.

- Projected decals ([ADR-092](adr/092-projected-decals.md)) change base colour
  only. Project packaging does not
  lower or package a material path inside a component, for decals and brush
  faces alike. The desktop decal cost has local, non-authoritative
  measurements only.
- Bistro shadows depend on a scene's entity count. On the Vulkan street view,
  adding six entities that draw nothing moves sun-shadow edges, changes
  lighting near the street lamps and lowers `Shadow.LocalMask` by about
  0.44 ms, with depth identical (ADR-092, Desktop evidence). The cause is
  not investigated.

- New viewport camera/grid controls, text sizing and inspection modes pass the
  Release editor build with both production shader compilers. Inspection modes
  render on the tiled pipeline (ADR-087) and pass a Release Vulkan snapshot and
  Debug validation on Windows
  ([ADR-044](adr/044-shader-cross-backend-contract.md#editor-inspection-views)).
  No timing claim is made.

- Physics authoring includes cooked convex/triangle collision, parented bodies,
  bone attachments/ragdolls, joints, sweeps, contacts and named layer matrices.
  AVBD/fracture, deforming collision, rotational sweeps and active-ragdoll blending
  remain future work. Native Vulkan/Windows, skinned-ragdoll renderer captures and
  matched Release cost evidence remain separate gates;
  [ADR-072](adr/072-entity-collision-and-rigid-body-physics.md) records the limits.

- The complete asset/application ownership split and preparation of all native
  pass families passed Release app/editor builds, CPU checks, and serial Metal
  API-validation draw/resize cases. Bistro depth and work counts match the
  baseline; color variation remains. ADR-004 records these evidence limits.
  That ownership series had compile-only Vulkan coverage; later focused Vulkan
  audit checks are recorded below.
- The renderer audit fixes passed Windows Release and CPU builds plus a focused
  native Vulkan synchronization-validation capture. Shear, static HZB and all
  eight jitter phases at an odd extent match their culling-disabled reference depth;
  mirrored instances match reference depth with small color differences. Bistro
  retains identical depth; 21 of 480,000 color pixels differ, at most 3/255.
  Completed steady-state slots upload zero geometry-table bytes. Raw float16
  bloom captures across two, four and six levels differ by one output ULP.
  The audit recorded no native Metal check. Transmission redesign, draw sorting
  and graph caching remain
  unimplemented: local measurements do not establish a sufficient benefit.
- Resource preparation, native object/encoder creation, command-buffer begin/end,
  acquisition, submission and completion remain fallible. Prepared command
  emission uses proven data and `void` recorders.
- Metal present-target recreation retains its fixed three-image capability and
  ignores requested image counts; cases requiring two images are unavailable.
- Metal's per-geometry submesh storage removes the former 512-range limit.
  The node-preserving Bistro cache contains 646 ranges. Native loading,
  API-validated stop/resize/resume and Release retained-image captures pass.
  Shared GPU geometry buffers now reuse completed vertex/decode and index spans
  while preserving persistent defaults and retaining high-water backing.
  A serial Metal API validation run alternates a small glTF fixture with added
  shapes and back twice: each unload restores the same live default ranges,
  backing capacity stays fixed, and final teardown releases every range without
  resource warnings. Material slot reuse preserves live materials after reload.
  Native evidence remains separate from allocator tests.
- One geometry capacity, `VKR_GEOMETRY_CAPACITY` (32,768 since 2026-10-11,
  from 16,384), sizes the geometry system's ID space, Metal's mesh table and
  Vulkan's publication records. Each brush material group and each terrain
  tile is a geometry, so a 45,000-entity level with two terrains passed 16,384
  geometries while its mesh and instance counts stayed far lower, and brushes
  past the bound built no mesh. The raise grows Metal's capacity-sized
  geometry-row upload span by 640 KiB (it still writes only reachable rows) and
  Vulkan's publication records by about 4 MiB. It has no matched Bistro timing
  and no native Vulkan check.
- Interactive editor runs have produced two macOS AGX firmware data aborts with
  the same fault signature. The second occurred with Scene rendering stopped
  and Metal API/shader validation disabled; the editor stack was waiting for
  command-slot completion. The triggering GPU operation remains unidentified.
  The triggering operation remains unresolved; bounded passing captures
  do not establish long-session editor stability.
  Offline review corrected missing window-layer residency registration and
  premature memory collection in publication-failure cleanup. These are concrete
  contract defects; their relationship to the panics remains unverified. An opt-in,
  bounded Metal diagnostic log now records CPU submission and lifetime events
  ([ADR-051](adr/051-renderer-harness-and-evidence.md#metal-crash-diagnostics)).
  After two passing tiny diagnostic cases, a serial API-validated Bistro startup
  produced a system watchdog panic. Demand-created heaps and a managed cap now
  replace the fixed resident heap. Demand-grown ICBs let the bounded Bistro
  case pass under 4 GiB; larger interactive configurations can still exhaust
  the managed cap. Geometry ranges are reused after completion. A later tiny
  UI-only run with a reduced test cap produced an IOGPUFamily kernel data abort
  and a 13.37 GiB editor resident-memory observation. Missing autorelease pools
  were corrected afterward. Subsequent bounded normal Release checks pass full-size
  Bistro Stop/resize/Resume and unload/reload. Resolution fallback and bounded
  texture retries finish both loads without terminal texture failures; the UI
  stays responsive. Demanded missing/evicted gauges now prevent false readiness,
  and asynchronous requests retain published textures through GPU completion.
  Charged texture-capacity accounting and bounded retries correct the later
  automatic-budget feedback loop. Demand-sized graph draw tables and budget
  sampling after publication remove further avoidable pressure. Metal retries
  Scene image allocation once at the same requested extent after completion-gated
  reclamation of superseded targets. A bounded two-load Bistro editor check keeps
  1528×1074 output with all 517 texture assignments resident and zero missing,
  pending, failed or evicted textures; the dynamic-resolution controller still
  varied internal resolution. Failed upward tiers now require measured
  headroom before another probe, limiting repeated resizing for unchanged work.
  Recovered Scene-image allocation attempts emit one warning; terminal failures
  retain detailed errors. A separate full-target spatial check passes
  actual 1528×1074 internal rendering under the unchanged 4 GiB managed cap.
  Native Vulkan acceptance and panic causality remain unresolved; see
  [ADR-046](adr/046-editor-viewport-mapping-and-picking.md#verification-and-limits)
  for the distinct failure and memory evidence.
- Deformation/procedural/particle motion and broad animation/disocclusion coverage
  remain outside the completed rigid-motion temporal contract.
- Visibility-buffer MSAA, a general effects system, asynchronous graph queues
  and fully graph-declared IBL baking are not production features.
- Sparse baked diffuse volumes bake on Vulkan ray queries, Metal ray
  tracing or the CPU and light both pipeline classes; light paths are traced
  per layer; see
  [ADR-054](adr/054-baked-diffuse-volumes.md). The desktop pipeline keeps
  active lamp groups apart from sky light on every frame; its native Vulkan
  indirect diffuse and local cost are checked, and the reflection change in
  a sealed lamp-lit room is not.
- Lightmap sets bake on Metal ray tracing or Vulkan ray queries into VKLM
  files that projects store and package; each platform bakes only the
  planes its pipeline class samples, and a scene names one set per class
  (`lightmaps.tiled`, `lightmaps.desktop`). Lowering keeps the host's class,
  and the loader checks the set's plane table before it reads the pages.
  The tiled pipeline samples every layer; the desktop pipeline samples the
  lamp groups' desktop planes in place of its static lamps in opaque and
  transmission shading, moving receivers take the volume's lamp light, and
  only the two baked lamps strongest at the moving casters cast their
  shadows; see [ADR-088](adr/088-baked-lightmap-sets.md) and
  [ADR-104](adr/104-desktop-baked-lamps.md). The per-class references pass
  the Debug CPU suite; on native Vulkan a fixture loads its
  `lightmaps.desktop` set and a project scene that names only
  `lightmaps.tiled` reads no set. A Windows bake that keeps the tiled set
  and a Mac load after a depot pull have not run.
- Charlie sheen is implemented below clearcoat in runtime and offline lighting.
  Its two-component rectangle fit retains measured errors for dim tilted lights;
  [ADR-063](adr/063-charlie-sheen.md) records those approximation limits and the
  unavailable native Vulkan comparison.
- Profiled surface diffusion is a planar surface approximation. Nearby folded or
  stacked sheets of one object/profile can exceed its normalized area budget; it
  does not model finite-solid transmission. Runtime support is screen-space and
  bounded, and extreme source irradiance truncates at the half-float limit. Native
  Vulkan execution remains unavailable; see
  [ADR-068](adr/068-profiled-surface-diffusion.md).
- Arbitrary indirect-light occlusion outside valid baked-volume
  coverage, meshlets and shader hot reload are absent. The GPU LOD table serves
  terrain tiles and cooked static meshes' levels, whose open borders stay locked
  ([ADR-085](adr/085-gpu-geometry-lod-and-terrain-geomorphing.md)).
- Imported scene documents, the
  World and added scenes do not stream by cells
  ([ADR-086](adr/086-world-partition.md),
  [audit](proposals/level-toolkit-audit.md)).
  [Meshlet cluster culling](proposals/meshlet-cluster-culling.md) records the
  measured geometry-bound cost and why finer culling has not paid yet.
- Native evidence remains incomplete for transmission, visibility and packed
  geometry, punctual lighting and shadow transitions on Vulkan, and for
  tonemap, UI/text color, coverage, picking and mixed-DPI output on both
  backends.
- The Graphics Settings and twelve-recipe Bakery integration is source-integrated.
  Settings CPU oracles pass two round trips, twenty invalid/default and
  dependency cases, restart/live classification, and missing-file handling;
  the process-group cancellation oracle also passes. Release and Debug wrappers
  pass without cooking log entries; a third Release editor-wrapper pass also
  passes, and all four shared-table SHA-256 values remain unchanged. A native
  macOS Graphics check passes with exit code 0, covering the sole Graphics menu
  item, all five left tabs, right pane, live controls, persisted values, Restore
  defaults, and the display Vsync restart notice. Bakery UI coverage passes the
  GGX Done/exit-0 `DFG unchanged` result and cancelled anisotropy job with no
  cooker descendants; final UI opacity coverage also passes. Windows
  UI/process-tree behavior and native Vulkan acceptance remain unavailable; see [ADR-027](adr/027-immediate-mode-grid-ui.md)
  and the [Windows/Vulkan verification checklist](proposals/windows-vulkan-verification.md).
- SH needs deterministic GPU projection fixtures, local-probe quality review,
  submitted-frame lifetime stress and a valid comparative performance record.
- SMAA T2x with skinned or procedural motion, final-color baseline
  acceptance and authoritative post-effect/reconstruction performance require
  their own matched Vulkan evidence. No tiled-pipeline Bistro baseline is
  accepted yet.
- SMAA T2x and motion blur read the immediately preceding frame's
  history, but cloud history without T2x and froxel fog history still take
  the newest completed producer, so their history age follows GPU timing
  ([ADR-037](adr/037-portable-same-resolution-temporal-antialiasing.md)).
- Native evidence for the 2026-10-12 removal of SSR, SSGI and FSR 3.1
  (builds, Bistro captures and timing) is pending.
- The temporal shader corrections and stationary coverage support pass
  bounded Vulkan Release Bistro profiling and static/moving-camera snapshots
  on RX 6700 XT after fixing cooker memory growth, upload-memory fallback and
  harness stack exhaustion. The earlier host freeze cause remains
  unconfirmed. Focused Vulkan synchronization validation passes; matched
  speedup measurements and broader moving-image quality acceptance remain
  open gates.

[ADR-044](adr/044-shader-cross-backend-contract.md) maps source counterparts and
defines the native evidence each backend owes. Builds, static source review and
one backend's success do not prove the other backend's behavior or
performance.
