---
status: implemented
updated: 2026-09-05
authority: architecture
---

# Renderer architecture

VKR is a C11 renderer with Metal 4 on macOS and capability-gated Vulkan 1.4 on
Windows. Both consume versioned packets and one authored render graph. Native
implementations own GPU resources, pipelines, commands and completion; shared
code owns portable contracts and scene-facing systems. Linux, D3D12 and the
retired Vulkan 1.2 renderer are not current execution paths.

This document describes code present on 2026-09-05. It does not certify a fresh
native run or a performance result. [INDEX](INDEX.md) locates accepted decisions
and proposals; [CONTEXT](CONTEXT.md) defines vocabulary.

## Ownership and source map

| Owner | Responsibilities | Source |
|---|---|---|
| Application/runtime | Lifecycle, frame input, packet assembly, sample scenes | `lib/src/application.h`, `runtime/` |
| Frontend | Scene-facing subsystems, renderer selection, packet validation | `lib/src/renderer/renderer_frontend.c` |
| Selected implementation | Native resources/pipelines, graph realization, record/submit/cancel, targets | `lib/src/renderer/metal/`, `vulkan/` |
| Shared graph | JSON realization, dependency order, culling, subresource barriers | `lib/src/renderer/vkr_rg_json.c`, `vkr_rg_compile.c` |
| GPU lifetime cores | Ranges, submit values, generation slots, ABI, capture requests | `lib/src/renderer/vkr_gpu_*`, `vkr_capture_ring.*` |
| Scene and assets | ECS scene, mesh bridge, loading, materials, lights, text and UI | `lib/src/renderer/systems/`, `resources/loaders/` |
| Production shaders | Shared math and native bindings/entry points | `lib/src/renderer/shaders/` |
| Offline tools/harness | Asset cooking, cases, captures, comparisons and profiles | `tools/` |

The application and editor are independent targets over `renderer_lib` and the
neutral sample runtime. The app owns its F6 debug overlay; the editor owns its
dock composition and startup `--scene-only` mode. Neither executable imports the
other's source. Editor details are in [ADR-027](adr/027-immediate-mode-grid-ui.md).

`VkrRendererImpl` selects coarse lifecycle and frame operations once. There are
no per-draw backend-table calls, frontend pipeline registry, generic command RHI,
legacy adaptor or public render-pass object system. Assets publish through
`VkrAssetPublisher`; [ADR-025](adr/025-selected-renderer-implementation-strategy.md)
and [ADR-024](adr/024-shared-bindless-gpu-cores.md) define the boundary.

## Frame protocol

1. `vkr_renderer_prepare_frame()` proves frame-slot reuse and prepares/acquires
   the selected target.
2. Render-thread resource finalization publishes ready asset generations.
3. The application assembles `VkrRenderPacket` and borrowed arrays in scratch.
4. `vkr_renderer_submit_packet()` validates structural input, realizes the graph,
   records native work, submits, captures and presents through the implementation.
5. Completed submission results feed timing, readback, retirement and history.

Packet version 27 groups frame globals and optional world, shadow, skybox, UI,
editor, picking, text and debug payloads. Globals include exposure, bloom, GTAO
and temporal controls. Arrays remain caller-owned until submit returns; handles
refer to retained registries. Version, capacity, pointer/range and generation
checks belong before lowering; recording consumes producer-proven rows.

Preparation precedes packet validation and may acquire a target or begin command
recording. Rejection/failure cancels through the selected implementation.
Residency, retained graph contents and histories commit only after successful
submission. Retained asset/text mutations are not a general transaction. Packets
are not standalone serializable replay objects. See
[ADR-004](adr/004-stateless-render-packet.md).

## Scene extraction and publication

`VkrWorld` owns archetype ECS state and queries. `VkrScene` adds hierarchy,
transforms, resource references, lights, environment/probes, text and render IDs.
`vkr_scene_handle_sync()` mirrors render-facing changes into `VkrMeshManager`;
application packet assembly scans mesh/instance/submesh records rather than
reading ECS archetype arrays directly. This bridge duplicates some retained data.

Extraction counts static and total candidates before allocation, then fills
disjoint static/dynamic spans in one source traversal. Transmission and direct
ordinary-blend side streams retain source encounter order. Stable identities
and static/dynamic/publication generations
let backend slots preserve unchanged static rows. Conservative camera culling
and back-to-front sorting apply to ordinary blend; opaque/cutout/transmission
and shadow classification run on the GPU.

Direct-draw preparation resolves live generations and submesh ranges before
native encoding. Vulkan omits pending geometry/material publication and preserves
ready draw order; invalid or stale references still reject the packet. Metal has
no pending native handle and rejects absent/stale references. Both encode prepared
rows and reserve each pass's root span before its draw loop; picking and blend
roots remain disjoint. See [ADR-004](adr/004-stateless-render-packet.md).

Workers perform CPU-only resource preparation. Render-thread finalization owns
GPU publication; its upload budgets allow an oversized first upload to progress.
Required dependency/publication failure prevents scene activation.
Materials initially publish semantic defaults and request textures incrementally;
ready textures replace material rows. Shared texture residency counts each GPU
texture once and completion-retires resources only after their last resident
reference leaves. Loader/cooker decisions are in
[ADR-030](adr/030-offline-mesh-optimization-and-cooking.md) and
[ADR-017](adr/017-prepared-specular-glossiness-lowering.md).

UI uses an immediate-mode API over a retained cache, grid layout and one bounded
indexed/scissored stream. Text/font systems own glyph layout resources and
cooked VKFA font loading; canonical derivative MTSDF coverage and DPI-derived
pre-layout sizing are shared contracts. MTSDF atlas sampling stays linear when
scene-texture filtering changes. The app debug overlay has 11/13-device-pixel
minimum title/body sizes; its authored 9/11-point sizing still governs at higher
content scales. See [ADR-027](adr/027-immediate-mode-grid-ui.md)
and [ADR-034](adr/034-offline-cooked-font-artifacts.md) through
[ADR-036](adr/036-dpi-derived-ui-text-scale.md). Resource worker/finalize ownership
is in [ADR-045](adr/045-resource-prepare-and-render-thread-finalize.md); stable
queued event payloads and latest-value resize delivery are in
[ADR-047](adr/047-event-payload-and-resize-mailbox-lifetimes.md).

## Graph and native execution

Both implementations parse
[`main.rendergraph.json`](../assets/render_graphs/main.rendergraph.json), resolve
conditions, aliases, names and repeats per submitted frame, and use the shared
compiler for dependencies, ordering, culling and barriers. Native executor
registries bind the authored operations, including conditional MetalFX declarations.
Vulkan rejects active MetalFX passes during graph validation; disabled declarations
do not block startup. There is one GPU-driven world topology;
no retained-forward/legacy world branch remains.

The graph describes image reads/writes/attachments, buffer access, compute
and indirect dispatches, and transfer uses. Image state is tracked per mip/layer;
compatible same-pass accesses combine before the barrier, and incompatible
layouts fail compilation. Same-layout writes still generate hazards. Buffer
barriers cover whole buffers. Compute/transfer pass types use the graphics
submission path and do not implement asynchronous queues or ownership transfer.

Native backend caches reuse graph resources until their resolved descriptions change.
Vulkan replacement waits for the greatest submitted use of the changed resource's
instances before freeing their views, descriptors and storage. Editor Scene-panel
resizes use this boundary independently of swapchain recreation.
`TRANSIENT` contents are frame-local, backed by reusable overlap-safe allocations;
transient aliasing is absent.
History owners select completed compatible instances. `RETAINED` image contents
have per-instance, per-subresource validity, seeded from submitted terminal state
and committed only after successful submit. Invalid retained reads fail graph
compilation. Cached allocation and `PERSISTENT` are not content proofs.

`IBL.Bake` is an authored uncullable compute pass, but its nested resource
accesses and barriers remain backend-owned. Uploads and portions of
capture/presentation likewise remain outside complete graph resource declarations,
with explicit native barriers and completion ownership. See [ADR-002](adr/002-render-graph.md) and
[ADR-029](adr/029-retained-graph-resources.md).

## Rendering pipeline

The active graph conditions select direct or editor presentation, optional picking,
post controls and the temporal consumer. The main dataflow is:

1. Publish transforms/candidate tables, classify camera and cascade views, compact
   visible rows, and encode Metal ICB or Vulkan indirect-count commands.
2. Raster opaque/cutout visibility and depth, build HZB, and peel four ordered
   transmission visibility layers.
3. Resolve the G-buffer, evaluate GTAO, compute HDR lighting, and shade transmission
   from deepest to nearest. Resolve requested picking, then draw ordinary blend.
4. Reconstruct temporal Scene HDR through portable TAA or selected MetalFX.
5. Meter exposure, produce/combine bloom, tonemap/FXAA and compose native UI.

Shadow passes produce directional cascades when their retained reuse proof fails.
Source topology and submission policy are in
[ADR-028](adr/028-gpu-driven-deferred-visibility-buffer.md).

Static geometry uses 32-byte packed vertices and a 32-byte range decode record,
with float32 UVs. Cooking and publication validate range quantization; there is
no selectable 24-byte float16-UV mode. See
[ADR-031](adr/031-versioned-packed-static-geometry-abi.md).

Source instances remain 80 bytes; native publication/upload prepares 128-byte
instances with inverse-transpose normal directions and mirror handedness.
Tangents retain model-linear transport. Baked glTF import uses the same distinction;
cooked mesh version 16 rejects older tangent-transform results. See
[ADR-044](adr/044-shader-cross-backend-contract.md) and
[ADR-030](adr/030-offline-mesh-optimization-and-cooking.md).

Native depth-writing passes use strict less-than tests; blend/text depth reads
accept equality. Both cull with transformed-axis sphere scale and retain odd
source edges during HZB reduction. Transmission compaction uses native subgroup
identity instead of assuming a workgroup invocation mapping.

PBR materials use prepared metallic-roughness data with retained dielectric
response from specular-glossiness conversion. Texture color/data interpretation
is resolved during loading. Transmission preserves reflection/emission, replaces
diffuse, handles texture-driven thickness/attenuation and samples an ordered
background or an immutable opaque roughness pyramid. Four layers are the bounded
production policy; a fifth layer is diagnostic. See
[ADR-018](adr/018-graph-declared-transmission-feedback.md).

Punctual lighting uses a stable 128-light table and 384-cell fragment-local
bitmask grid with exact range/cone rejection. Up to 16 ready probes contribute
fragment-space AABB weights. Only directional lighting samples CSM; light ranges,
probe bounds and GTAO do not establish arbitrary wall/furniture occlusion.
See [ADR-019](adr/019-bounded-forward-spatial-lighting.md).

HDR source conversion, skybox and GGX prefilter use cubemaps. Diffuse lighting
uses nine GPU-resident L2 coefficients for `E/pi`, with a black sentinel and
completion-safe replacement slots. IBL bake work is not fully graph-declared.
See [ADR-016](adr/016-hdr-environment-format.md) and
[ADR-038](adr/038-sh-l2-diffuse-irradiance.md).

Directional shadows default to four cascades with snapping, fit hysteresis,
per-target-image reuse and shared PCF/bias units. Static reuse requires guard
containment, matching generations, valid retained layers and a match with the
common submitted fit; stale physical copies redraw that fit once. Dynamic overlap or
incomplete publication forces rendering. SDSM and proactive refresh are opt-in;
fixed splits and zero proactive budget remain defaults. See
[ADR-041](adr/041-retained-cascaded-shadows.md) and
[ADR-033](adr/033-occupied-depth-sdsm-feedback.md).

Portable TAA consumes rigid motion, stable instance/submesh identity, depth and
reactive composition. Current color is reconstructed onto the unjittered grid;
history validation accounts for raw metadata's jittered footprint. Opaque/background
coverage can survive camera motion while its surface, depth and motion remain
supported nearby; partial support reduces history confidence. Temporal history uses
the preceding submitted frame with GPU dependencies, and output reuse still
proves completion. Fully supported 4x4 color footprints use clamped cubic
reconstruction; rejected edges keep masked bilinear sampling. Resets exclude
every pre-reset producer. A packet-content signature plus native resource and
graph revisions enables accumulation without coverage clipping for unchanged
scenes, including static glass and thin geometry absent in one jitter phase.
Pending writers and unsupported text disable that path; any content or camera
change restores normal rejection. After 128 unchanged samples, nonreactive pixels
retain their completed value exactly. Sample age uses the existing depth-history
spare channel, with no additional images. Retained shadow images converge to a
common submitted projection so cached per-image fits cannot prevent temporal
convergence after camera movement. The G-buffer writes
sky rotation motion for portable TAA and MetalFX. Completed history remains
scene-linear and exposure-independent.

Automatic exposure adapts over elapsed time since its selected completed state,
using a renderer-owned committed exposure clock and a bounded hitch policy.
Defaults lower exposure at 8 EV/s and raise it at 1 EV/s with a +4 EV upper target
limit. Exposure, bloom and GTAO have independent packet bypasses. GTAO's slice
basis and horizon signs follow view reconstruction and affect indirect diffuse
only. Direct lighting and the IBL PDF share the unclipped supported GGX lobe.
See [ADR-037](adr/037-portable-same-resolution-temporal-antialiasing.md)
and [ADR-042](adr/042-scene-linear-post-processing.md).

## Presentation and platform boundaries

Windows uses Per-Monitor V2 physical client pixels. Final shaders emit linear RGB
into sRGB attachments; UI/text authored colors decode once before linear blending.
Internal Scene pixels, Scene presentation pixels and physical target/UI pixels
remain distinct. Picking and composition share viewport mapping.
See [ADR-043](adr/043-presentation-dpi-and-color-transfer.md).

Vulkan requires the explicit descriptor-buffer/device feature profile and rejects
unsupported devices; a Vulkan 1.4 version string alone is insufficient. There
is no legacy descriptor-set fallback. See
[ADR-023](adr/023-vulkan-1-4-bindless-capability-profile.md).
Requested material anisotropy uses the enabled device feature and effective
limit (up to 16); devices without it report a maximum of 1.

Metal supports explicit internal scale and MetalFX temporal reconstruction.
The sample selects dynamic MetalFX in direct and paneled modes; zero-initialized
renderer API callers use unit-scale spatial mode. Vulkan rejects non-unit scale
and MetalFX. UI stays native after Scene reconstruction. MetalFX motion targets
the exact preceding scaler encode, with GPU event/fence ordering. Under Metal
validation, the sample explicitly uses portable TAA/spatial diagnostics because
the installed native MetalFX wrappers are incompatible. See
[ADR-039](adr/039-metal-internal-render-scale.md) and
[ADR-040](adr/040-metalfx-temporal-dynamic-resolution.md).

## Memory, synchronization and observability

CPU storage uses arenas for bulk lifetimes, DMemory for individual release and
pools for fixed-size churn. Borrowed views can be invalidated by capacity growth;
allocator synchronization is explicit. Vulkan driver host allocations use null
callbacks and are outside VKR CPU totals. See
[ADR-006](adr/006-cpu-memory-allocators.md).

Container creation and growth report failure at their owning boundary. Failed
vector/hash growth preserves the previous contents and capacity; callers reserve
known batches before population and propagate allocation failure through existing
load, build or initialization errors. Partial application startup unwinds acquired
owners, and failed text rebuilding preserves its previously published layout.

Vulkan pools keyed device/upload/staging/readback blocks, with persistent mappings and
required dedicated-allocation exceptions. Completion-protected Vulkan frame slots
keep directly read uploads separate from copy-only candidate staging; both retain
capacity grown during packet preflight. Metal uses placement heaps and native
upload/readback adapters. Shared cores track logical ranges, generations, submit
values and retirement; physical allocations remain native. No VMA, online
defragmentation, GPU heap eviction or transient aliasing is implemented.

Slot and resource reuse require their actual last-submit completion. Vulkan
submission uses timeline values, while window presentation uses per-image
semaphores and either maintenance present fences or completed reacquire-wait
submissions. Metal maps completion to native command submission and event ordering.
Lifecycle changes may wait idle; ordinary successful frames do not wait the whole
device. Capture/picking results publish asynchronously and require release.
See [ADR-009](adr/009-frame-synchronization.md) and
[ADR-014](adr/014-offscreen-present-target.md).

Both backends publish completed GPU timing/results and allocation/visibility
metrics. Unsupported timing scopes are unavailable, never zero-duration proof.
Metal compute/graphics timestamps exist; transfer timing is not supported.
The harness owns case identity, artifacts, comparison and performance authority.
After resource/bootstrap readiness it starts authored warmup at the common zero
of raster jitter and GTAO noise, with temporal history invalidated; replay version
4 fingerprints this behavior.
See [ADR-015](adr/015-metrics-module.md) and [ADR-051](adr/051-renderer-harness-and-evidence.md).

## Remaining implementation and evidence boundaries

These are limits of current code or retained acceptance, not scheduled promises:

- Metal present-target recreation retains its fixed three-image capability and
  ignores requested image counts; cases requiring two images are unavailable.
- Deformation/procedural/particle motion and broad animation/disocclusion coverage
  remain outside the completed rigid-motion temporal contract.
- Visibility-buffer MSAA, terrain, a general effects system, asynchronous graph
  queues and fully graph-declared IBL baking are not production features.
- Clearcoat, sheen, point/spot shadows, arbitrary local-light occlusion, meshlets,
  automatic mesh LOD and shader hot reload are absent.
- Native source exists for both backends, but same-revision crossed transmission,
  visibility/packed geometry, punctual lighting, shadow-transition, tonemap,
  UI/text color/coverage/picking and mixed-DPI evidence remains incomplete.
- Near-degenerate barycentric rejection and zero interpolated tangent handedness
  still have different native edge policies, recorded in ADR-044.
- SH needs deterministic GPU projection fixtures, local-probe quality review,
  submitted-frame lifetime stress and a valid comparative performance record.
- Moving TAA/MetalFX quality, final-color baseline acceptance and authoritative
  post-effect/reconstruction performance require their own matched evidence.
  Portable Metal validation does not certify native MetalFX.
- The shader corrections and stationary coverage support remain UNALIGNED under
  ADR-044. Bounded Vulkan Release Bistro profiling and static/moving-camera
  snapshots pass on RX 6700 XT after fixing cooker memory growth, upload-memory
  fallback and harness stack exhaustion. The earlier host freeze cause remains
  unconfirmed. Metal execution and bilateral capture comparison are unavailable
  on that Windows host. Focused Vulkan synchronization validation passes;
  matched speedup measurements and broader moving-image quality acceptance
  remain open gates.

[ADR-044](adr/044-shader-cross-backend-contract.md) maps source counterparts and
defines parity evidence. Builds, static source review and one backend's success
do not prove bilateral native compatibility or performance.
