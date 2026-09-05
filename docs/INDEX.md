# Documentation index

[ARCHITECTURE.md](ARCHITECTURE.md) describes the current renderer and its known
boundaries. [CONTEXT.md](CONTEXT.md) defines project vocabulary. Code and its
production callers are the implementation authority; ADRs explain decisions,
and proposals describe future work.

This inventory covers every retained document. ADR numbers remain stable;
removed numbers are not reused.

## Build and run

The app and editor are separate executables using `runtime/` and `renderer_lib`.
Build through the repository wrappers, which also compile shaders and cook assets:

```sh
./build_release.sh
./build_editor.sh Release
```

On Windows, use `build_release.bat` or `build_editor.bat Release`. Set
`VCPKG_ROOT` to your vcpkg checkout and install the font cooker's dependency:

```powershell
& "$env:VCPKG_ROOT/vcpkg.exe" install freetype:x64-windows-static
.\build_release.bat
```

CMake uses that checkout's toolchain and defaults to `x64-windows-static`,
including its static C runtime. Explicit toolchain and triplet settings take
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
| [004](adr/004-stateless-render-packet.md) | Versioned packet submission with ordered preparation | implemented |
| [006](adr/006-cpu-memory-allocators.md) | CPU allocation by lifetime | implemented |
| [009](adr/009-frame-synchronization.md) | Separate submission and presentation completion | implemented |
| [010](adr/010-ecs-scene-system.md) | ECS-owned scene state with a retained render mirror | implemented |
| [012](adr/012-texture-compression-pipeline.md) | KTX2/UASTC texture artifacts with capability-selected transcode | implemented |
| [014](adr/014-offscreen-present-target.md) | Window and offscreen targets share frame submission | implemented |
| [015](adr/015-metrics-module.md) | Bounded typed metrics and pinned snapshots | implemented |
| [016](adr/016-hdr-environment-format.md) | HDR source delivery and cubemap sampling | implemented |
| [017](adr/017-prepared-specular-glossiness-lowering.md) | Prepare PBR materials before publication | implemented |
| [018](adr/018-graph-declared-transmission-feedback.md) | Ordered transmission with declared feedback | implemented |
| [019](adr/019-bounded-forward-spatial-lighting.md) | Bounded punctual lighting and local probes | implemented |
| [023](adr/023-vulkan-1-4-bindless-capability-profile.md) | One explicit Vulkan capability floor | implemented |
| [024](adr/024-shared-bindless-gpu-cores.md) | Shared allocation, publication and completion cores | implemented |
| [025](adr/025-selected-renderer-implementation-strategy.md) | One coarse selected renderer implementation | implemented |
| [027](adr/027-immediate-mode-grid-ui.md) | Immediate-mode grid UI with retained CPU state | implemented |
| [028](adr/028-gpu-driven-deferred-visibility-buffer.md) | One GPU-driven world topology | implemented |
| [029](adr/029-retained-graph-resources.md) | Retain submitted image contents per subresource | implemented |
| [030](adr/030-offline-mesh-optimization-and-cooking.md) | Versioned meshoptimizer-cooked mesh artifacts | implemented |
| [031](adr/031-versioned-packed-static-geometry-abi.md) | One 32-byte packed static vertex ABI | implemented |
| [032](adr/032-two-phase-confirmed-visibility.md) | Keep exact one-phase visibility gates | declined |
| [033](adr/033-occupied-depth-sdsm-feedback.md) | Optional occupied-depth shadow fitting | implemented |
| [034](adr/034-offline-cooked-font-artifacts.md) | Cooked MTSDF font artifacts | implemented |
| [035](adr/035-canonical-mtsdf-screen-pixel-range-shading.md) | Derivative-based MTSDF coverage | implemented |
| [036](adr/036-dpi-derived-ui-text-scale.md) | Window content scale before UI layout | implemented |
| [037](adr/037-portable-same-resolution-temporal-antialiasing.md) | Portable scene-linear temporal antialiasing | partial |
| [038](adr/038-sh-l2-diffuse-irradiance.md) | GPU-resident L2 diffuse response | implemented |
| [039](adr/039-metal-internal-render-scale.md) | Separate internal Scene and physical output extents | implemented |
| [040](adr/040-metalfx-temporal-dynamic-resolution.md) | MetalFX temporal reconstruction and completed-GPU scale control | implemented |
| [041](adr/041-retained-cascaded-shadows.md) | Stable fits and retained directional shadow cascades | implemented |
| [042](adr/042-scene-linear-post-processing.md) | Scene-linear exposure, bloom and ambient visibility | implemented |
| [043](adr/043-presentation-dpi-and-color-transfer.md) | Physical-pixel presentation with one sRGB transfer | implemented |
| [044](adr/044-shader-cross-backend-contract.md) | Portable shader semantics with native ABI validation | implemented |
| [045](adr/045-resource-prepare-and-render-thread-finalize.md) | Worker preparation and render-thread resource finalization | implemented |
| [046](adr/046-editor-viewport-mapping-and-picking.md) | One editor viewport mapping for scene presentation and interaction | implemented |
| [047](adr/047-event-payload-and-resize-mailbox-lifetimes.md) | Event callback payload lifetime and coalesced resize handoff | implemented |
| [051](adr/051-renderer-harness-and-evidence.md) | Isolated harness runs and reviewed capture baselines | implemented |

## Proposals

These preserve unimplemented scope after removing shipped prerequisites and
retired API sketches. They are not scheduled commitments. Resolve their open
decisions before dependent implementation.

| Proposal | Scope |
|---|---|
| [Conditional D3D12 backend evaluation](proposals/d3d12-backend-evaluation.md) | Conditions for considering a third backend. |
| [Dedicated transfer queue](proposals/dedicated-transfer-queue.md) | Independent upload submission and completion-safe publication. |
| [Deformable scene effects](proposals/deformable-scene-effects.md) | A bounded deformation pilot with shared pass and history inputs. |
| [Editor UI extensions](proposals/editor-ui-extensions.md) | Advanced widgets, accessibility, and floating-window ownership. |
| [Graph-owned IBL baking](proposals/graph-owned-ibl-baking.md) | Declare queued bake resources and dependencies in the graph. |
| [Static-scene batching](proposals/static-scene-batching.md) | Evaluate static geometry merging against current GPU draw preparation. |
| [Terrain rendering](proposals/terrain-rendering.md) | Terrain data, tile ownership, LOD, and existing draw-path integration. |
| [Visibility-buffer MSAA](proposals/visibility-buffer-msaa.md) | Multisample visibility and resolve after a demonstrated quality need. |

## Maintaining this tree

Follow the [documentation skill](../.codex/skills/vkr-docs/SKILL.md). Keep current
behavior in the architecture and owning ADR, terms in the glossary, and future
features in proposals. Runtime graph and harness parsers plus checked-in inputs
own their contracts; there are no separately maintained descriptive JSON schemas
under `docs/`.
