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
`vkr_sample_runtime`. Repository builds compile shaders and required cooker tools
without running cooking. Explicit texture, font and mesh cooker wrappers build
their tool in `build_release` before invoking it. `VKR_FONT_COOKER_BUILD_DIR`
retains the font wrapper's custom-tree override. `build_test.sh` and
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

Release logging is stripped to its configured level for both app and editor.
Set `VKR_EDITOR_LOGGING=ON` in the environment before a build, or set the CMake
option explicitly, to compile detailed editor logging. This changes shared
library compilation and remains in the cache until explicitly set to `OFF`.
Selecting app versus editor does not change it. The
[build policy](ARCHITECTURE.md#build-policy) defines optimization and dependency
configuration; these settings alone do not establish measured performance.

Bakery owns mesh, font, texture, BRDF/table, reflection/probe and diffuse-volume
jobs.

When artifact regeneration is required, run Bakery or invoke the explicit
cooker wrapper. The main Bistro artifact includes its scene-specific light
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
| [012](adr/012-texture-compression-pipeline.md) | KTX2/UASTC texture artifacts with capability-selected transcode | implemented |
| [014](adr/014-offscreen-present-target.md) | Window and offscreen targets share frame submission | implemented |
| [015](adr/015-metrics-module.md) | Bounded typed metrics and pinned snapshots | implemented |
| [016](adr/016-hdr-environment-format.md) | HDR source delivery and cubemap sampling | implemented |
| [017](adr/017-prepared-specular-glossiness-lowering.md) | Prepare PBR materials before publication | implemented |
| [018](adr/018-graph-declared-transmission-feedback.md) | Ordered transmission with declared feedback | implemented |
| [019](adr/019-bounded-forward-spatial-lighting.md) | Bounded punctual lighting, cached transmitting local shadows and probes | implemented |
| [023](adr/023-vulkan-1-4-bindless-capability-profile.md) | One explicit Vulkan capability floor | implemented |
| [024](adr/024-shared-bindless-gpu-cores.md) | Shared allocation/completion cores and bounded Metal heap residency | implemented |
| [025](adr/025-selected-renderer-implementation-strategy.md) | Procedural renderer and prepared native commands | implemented |
| [027](adr/027-immediate-mode-grid-ui.md) | Grid UI, scene light labels and controls, dock stacks, Console and Bakery | implemented |
| [028](adr/028-gpu-driven-deferred-visibility-buffer.md) | One GPU-driven world topology | implemented |
| [029](adr/029-retained-graph-resources.md) | Retain submitted image contents per subresource | implemented |
| [030](adr/030-offline-mesh-optimization-and-cooking.md) | Versioned meshoptimizer artifacts preserving glTF nodes and shared geometry | implemented |
| [031](adr/031-versioned-packed-static-geometry-abi.md) | One 32-byte packed static vertex ABI | implemented |
| [032](adr/032-two-phase-confirmed-visibility.md) | Keep exact one-phase visibility gates | declined |
| [033](adr/033-occupied-depth-sdsm-feedback.md) | Optional occupied-depth shadow fitting | implemented |
| [034](adr/034-offline-cooked-font-artifacts.md) | Cooked MTSDF font artifacts | implemented |
| [035](adr/035-canonical-mtsdf-screen-pixel-range-shading.md) | Derivative-based MTSDF coverage | implemented |
| [036](adr/036-dpi-derived-ui-text-scale.md) | Window content scale before UI layout | implemented |
| [037](adr/037-portable-same-resolution-temporal-antialiasing.md) | Portable temporal antialiasing, bounded SSR-settling retention and checked static accumulation | partial |
| [038](adr/038-sh-l2-diffuse-irradiance.md) | GPU-resident L2 diffuse response | implemented |
| [039](adr/039-metal-internal-render-scale.md) | Separate internal Scene and physical output extents | implemented |
| [040](adr/040-metalfx-temporal-dynamic-resolution.md) | MetalFX reconstruction, stationary accumulation and completed-GPU scale control | implemented |
| [041](adr/041-retained-cascaded-shadows.md) | Stable fits and retained directional shadow cascades | implemented |
| [042](adr/042-scene-linear-post-processing.md) | Scene-linear exposure with completed-history time, bloom and directional ambient visibility | implemented |
| [043](adr/043-presentation-dpi-and-color-transfer.md) | Physical-pixel presentation, color transfer and image sharpness | implemented |
| [044](adr/044-shader-cross-backend-contract.md) | Portable shader semantics with native ABI validation | implemented |
| [045](adr/045-resource-prepare-and-render-thread-finalize.md) | Worker preparation and render-thread resource finalization | implemented |
| [046](adr/046-editor-viewport-mapping-and-picking.md) | Editor viewport mapping, transform gizmos, picking and retained Scene presentation | implemented |
| [047](adr/047-event-payload-and-resize-mailbox-lifetimes.md) | Event callback payload lifetime and coalesced resize handoff | implemented |
| [051](adr/051-renderer-harness-and-evidence.md) | Isolated harness runs, reconstructed HDR diagnostics and reviewed capture baselines | implemented |
| [052](adr/052-vulkan-fsr31-upscaling.md) | Vulkan FSR 3.1 temporal upscaling | implemented |
| [053](adr/053-energy-compensated-ggx.md) | Correlated Smith GGX and shared energy integration | implemented |
| [054](adr/054-baked-diffuse-volumes.md) | Offline multi-bounce diffuse volumes and runtime spatial diffuse lookup | implemented |
| [055](adr/055-screen-space-reflections.md) | Full-source opaque SSR, reflected-hit reprojection and incoming-radiance history | implemented |
| [056](adr/056-rectangular-ltc-lights.md) | One-sided rectangular LTC emitters, runtime lookup and baker transport | implemented |
| [057](adr/057-analytic-height-fog.md) | Analytic scene-linear height fog with ordered transmission composition | implemented |
| [058](adr/058-revision-baked-sky-atmosphere.md) | Revision-baked sky and global IBL with unified sun | implemented |
| [059](adr/059-froxel-volumetric-fog.md) | Bounded volumetric fog with independent scattering history | implemented |
| [060](adr/060-screen-space-diffuse-indirect-lighting.md) | Optional SSGI with synchronized history and jitter correction | implemented |
| [061](adr/061-extended-linear-display-output.md) | Optional EDR/scRGB output with OS headroom and SDR fallback | implemented |

| [062](adr/062-layered-clearcoat.md) | Independent clearcoat maps, layered GGX and coat-priority SSR | implemented |
| [063](adr/063-charlie-sheen.md) | Charlie sheen, bounded energy and shared rectangle tables | implemented |
| [064](adr/064-anisotropic-ggx-reflection.md) | Anisotropic GGX reflection, directional tables and material transport | implemented |
| [065](adr/065-thin-sheet-diffuse-transmission.md) | Thin-sheet diffuse backlighting and offline transport | implemented |
| [066](adr/066-post-reconstruction-depth-of-field.md) | Optional post-reconstruction lens blur | implemented |
| [067](adr/067-post-reconstruction-motion-blur.md) | Optional camera and rigid-object shutter blur | implemented |
| [068](adr/068-profiled-surface-diffusion.md) | Optional RGB surface diffusion and offline transport | implemented |

## Proposals

These preserve unimplemented scope after removing shipped prerequisites and
retired API sketches. They are not scheduled commitments. Resolve their open
decisions before dependent implementation.

| Proposal | Scope |
|---|---|
| [Renderer features and performance audit](proposals/renderer-features-perf/renderer-features-perf.md) | Normal/fog corrections, optional material storage, screen-space and post-processing costs, and remaining native acceptance. |
| [Conditional D3D12 backend evaluation](proposals/d3d12-backend-evaluation.md) | Conditions for considering a third backend. |
| [Dedicated transfer queue](proposals/dedicated-transfer-queue.md) | Independent upload submission and completion-safe publication. |
| [Deformable scene effects](proposals/deformable-scene-effects.md) | A bounded deformation pilot with shared pass and history inputs. |
| [Editor UI extensions](proposals/editor-ui-extensions.md) | Advanced widgets, accessibility, and floating-window ownership. |
| [Graph-owned IBL baking](proposals/graph-owned-ibl-baking.md) | Declare queued bake resources and dependencies in the graph. |
| [Windows/Vulkan verification checklist](proposals/windows-vulkan-verification.md) | Active Windows/Vulkan record: native renderer subset executed; bilateral Metal, cooker, HDR/DPI, and manual editor gates remain. |
| [Static-scene batching](proposals/static-scene-batching.md) | Evaluate static geometry merging against current GPU draw preparation. |
| [Terrain rendering](proposals/terrain-rendering.md) | Terrain data, tile ownership, LOD, and existing draw-path integration. |
| [Visibility-buffer MSAA](proposals/visibility-buffer-msaa.md) | Multisample visibility and resolve after a demonstrated quality need. |

## Maintaining this tree

Follow the [documentation skill](../.codex/skills/vkr-docs/SKILL.md). Keep current
behavior in the architecture and owning ADR, terms in the glossary, and future
features in proposals. Runtime graph and harness parsers plus checked-in inputs
own their contracts; there are no separately maintained descriptive JSON schemas
under `docs/`.
