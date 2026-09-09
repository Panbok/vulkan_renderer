---
status: partial
updated: 2026-09-09
authority: architecture
---

# Renderer architecture

VKR is a C11 renderer with Metal 4 on macOS and capability-gated Vulkan 1.4 on
Windows. Both consume explicit frame inputs and one authored render graph. Native
implementations own GPU resources, pipelines, commands and completion; shared
code owns portable contracts and scene-facing systems. Linux, D3D12 and the
retired Vulkan 1.2 renderer are not current execution paths.

This document describes code present on 2026-09-09. It does not certify a fresh
native run or a performance result. [INDEX](INDEX.md) locates accepted decisions
and proposals; [CONTEXT](CONTEXT.md) defines vocabulary.

## Library boundaries

The reusable libraries form an acyclic application boundary. `vkr_foundation`
provides common containers, memory, math and platform support from `lib/src/`.
`vkr_render_contracts` owns shared rendering values and CPU packing, tangent
and color-transfer helpers used by the renderer and offline tools. `renderer_lib` owns rendering and native backend work only: it
accepts frame data and an optional `VkrNativeSurface`, but has no window, input,
event, scene, loader or cooking owner.

`vkr_asset_formats` reads versioned cooked artifacts. It contains runtime
decoding, not source import or artifact encoding. `vkr_runtime` builds on the
renderer and format libraries. It supplies the reusable application host,
standard scene runtime, runtime core services, and scene-facing systems.
`vkr_sample_runtime` is an optional consumer that supplies sample control and
presentation policy for the app and editor. `vkr_asset_cooking` is tool-only;
it owns source import and cooked-artifact encoding and is not a runtime
dependency.

`assets/` contains asset data only. Shared reader code lives in
`runtime/src/assets/`, offline producers in `tools/assets/`, and renderer code
directly in `renderer/src/`.

Runtime image decoding, transcode caches and dynamic system-font rasterization
remain available. Offline mesh optimization, font atlas/MSDF generation and
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

## Ownership and source map

| Owner | Responsibilities | Source |
|---|---|---|
| Application host | Window, input, event dispatch, timing, shutdown and caller-state callbacks | `runtime/src/application/vkr_application_host.h` |
| Standard scene runtime | Scene, camera, lighting, shadows, UI, picking, resize events, frame scratch and input construction | `runtime/src/application/vkr_standard_scene_runtime.h`, `runtime/src/renderer/systems/vkr_scene_frame.c` |
| Renderer | Acquired-frame lifecycle, targets, derived frame data and native operation selection | `renderer/src/vkr_renderer.c`, `renderer/src/vkr_native_surface.h` |
| Selected implementation | Native resources/pipelines, graph realization, record/submit/cancel, targets | `renderer/src/metal/`, `vulkan/` |
| Shared graph | JSON realization, dependency order, culling, subresource barriers | `renderer/src/vkr_rg_json.c`, `vkr_rg_compile.c` |
| GPU lifetime cores | Ranges, submit values, generation slots, ABI, capture requests | `renderer/src/vkr_gpu_*`, `vkr_capture_ring.*` |
| Render assets | Geometry, textures, materials, meshes, fonts, persistent world text, loaders and load scratch | `runtime/src/renderer/systems/vkr_render_assets.c`, `runtime/src/renderer/resources/loaders/` |
| Production shaders | Shared math and native bindings/entry points | `renderer/src/shaders/` |
| Offline tools/harness | Asset cooking, cases, captures, comparisons and profiles | `tools/` |

The application and editor are independent targets over `vkr_runtime` and the
optional `vkr_sample_runtime`. The app owns its F6 debug overlay; the editor owns its
dock composition and startup `--scene-only` mode. Neither executable imports the
other's source. `core/vkr_subsystem_plan` resolves application boot dependencies;
the GPU renderer does not own that subsystem policy. The editor has tab stacking, layout persistence, keyboard focus and icon-only
independent simulation/render controls in a draggable Scene toolbar, alongside
load/unload and camera entry. Scene focus routes Tab to camera capture; panel
focus routes it to widgets. Hierarchy reads the authoritative scene
through a virtualized tree; Inspector sends typed selection and edit requests to
the runtime. Debug > Labels controls selectable directional, spot and point
light texture icons. The editor identifies ECS light components and projects
32-point labels above their origins using the packet's unjittered camera and
Scene mapping. Inspector exposes light enable, color/intensity, local direction
angles, punctual range and spotlight cone controls through the edit journal.
RMB holds free-camera capture; Tab/F3 and the toolbar remain toggle alternatives.
Console snapshots bounded structured logger history with a checkbox filter dropdown. Bakery runs
nine recipes—mesh, font, single texture, texture directory, GGX DFG, Charlie,
anisotropy, diffuse volume, and reflection probe—in one cancellable child
process at a time. Its setup, jobs and output views use labeled controls and
adapt to dock width; cancellation terminates the complete child process tree.
The diffuse recipe defaults to a tracked enclosed-room example; invalid-volume
bounds and child-baker diagnostics are reported in Bakery output.
Settings > Graphics has a left tab rail for Display, Quality, Lighting, Effects,
and Color and a clipped, scrollable right content area. The editor emits typed
`VkrGraphicsSettingsRequest` values; the sample runtime validates and owns their
application. Vsync, HDR, temporal upscaling, dynamic resolution, and render
scale changes show a restart-required notice. Other controls apply live and
invalidate the affected histories. Settings load from `VKR_GRAPHICS_SETTINGS_PATH`
or `.vkr-graphics-settings.json`, save after 0.25 seconds without another edit,
and flush on exit. Render Stop retains the last Scene image while UI continues;
Vulkan UI-only frames reset Scene readback copies before skipping absent producers.
Scene allocation failures trigger bounded output-resolution reductions while UI
resolution stays unchanged; an error at the minimum stops Scene retries.
The first Metal GPU completion timeout flushes diagnostics and terminates the
process without GPU teardown, because completion is unproven;
see [ADR-046](adr/046-editor-viewport-mapping-and-picking.md).
Editor details are in [ADR-027](adr/027-immediate-mode-grid-ui.md).
Transform editing is available through Inspector and world-axis gizmos. Gizmo
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
3. `vkr_renderer_render_frame(&frame, &input, ...)` validates input and target
   extent, derives private frame data, realizes the graph and prepares every
   enabled pass before recording native work, submitting, capturing and presenting.
4. Rendering consumes the acquired `VkrFrame`. Input-construction failure uses
   `vkr_renderer_cancel_frame(&frame)` to release acquired resources and
   recorded-but-unsubmitted work. Input rejection also cancels.
5. Completed submission results feed timing, readback, retirement and history.

`VkrFrame` identifies one acquisition and supplies resolved target facts. It must
not be copied or modified; its renderer must outlive it. Consumed or stale frame
contexts are rejected. Acquisition identity is separate from GPU completion.

Frame-input version 40 contains frame metadata, camera/lighting/settings and typed
world, shadow, skybox, baked diffuse-volume, rectangle-light, analytic-fog and
froxel-fog, UI, editor, picking and debug payloads. Supplied world-text and UI
streams are authoritative. `vkr_frame_input_validate()` checks structural input.
Private `VkrPreparedFrame` holds derived temporal, exposure, bloom, GTAO, SSR,
analytic-fog and froxel-fog values alongside the borrowed input; those derived
fields and text mutations are absent from the public frame input.

Arrays remain caller-owned until rendering returns. Retained assets use generation
identities and completion-protected storage. Acquisition precedes input validation;
rejection or recording failure must resolve acquired native resources. Residency,
retained graph contents and histories commit only after successful submission.
Earlier scene/text edits and resource publication are not a general transaction.
Frame inputs are not standalone replay recordings. See
[ADR-004](adr/004-stateless-render-packet.md).

## Scene extraction and publication

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
one scene-owned baked diffuse-volume binding, analytic fog, text and render IDs.
glTF nodes retain local matrices, names and source identities; source geometry is
shared across node instances, with decal variants where world-offset corrections
differ. Cooked mesh v17 retains the same source hierarchy. Source fingerprints
protect editor sidecar overrides against reimport conflicts. Inspector supports
TRS where the authored matrix is decomposable; sheared matrices remain exact and
read-only. The runtime owns selection and a bounded undo journal. UI borrows
scene data for its current build and never keeps ECS component pointers.
`vkr_scene_handle_sync()` mirrors render-facing changes into `VkrMeshManager`;
`vkr_scene_build_world_draws()` scans mesh/instance/submesh records through
explicit mesh, material, publication and view inputs rather than reading ECS
archetype arrays directly. This bridge duplicates some retained data.

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
pre-layout sizing are shared contracts. Immediate labels and buttons use common
font line metrics and align their baseline to physical pixels after DPI resolution.
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

Both implementations parse
[`main.rendergraph.json`](../assets/render_graphs/main.rendergraph.json), resolve
conditions, aliases, names and repeats per submitted frame, and use the shared
compiler for dependencies, ordering, culling and barriers.
`vkr_render_graph_prepare_frame()` derives portable conditions, shadow counts,
HZB/transmission/bloom/GTAO mip counts and GTAO constants once from the prepared
frame. Native formats, resource instances and history/completion selection remain
with each backend. Native executor registries bind the authored operations,
including conditional MetalFX and Vulkan FSR 3.1 declarations. Vulkan rejects
active MetalFX passes; Metal ignores inactive FSR declarations. Disabled
declarations do not block startup. There is one GPU-driven world topology;
no retained-forward/legacy world branch remains.

Shared native pass and timing storage covers the main graph's 163-pass maximum.
The no-TAA path can expand beyond either temporal upscaler because it restores
culling HZB generation. The graph-expansion test checks the full supported repeat
envelope before native emission; [ADR-025](adr/025-selected-renderer-implementation-strategy.md)
records the ownership and bound.

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

The active graph conditions select direct or editor presentation, optional picking,
post controls and the temporal consumer. The main dataflow is:

1. Publish transforms/candidate tables, classify camera and cascade views, compact
   visible rows, and encode Metal ICB or Vulkan indirect-count commands.
2. Raster opaque/cutout visibility and depth, build HZB, and peel four ordered
   transmission visibility layers.
3. Resolve the G-buffer, evaluate GTAO, and compute HDR lighting. When enabled,
   SSGI writes an isolated direct/emissive source, traces and filters its
   half-resolution diffuse residual, and composites it outside valid baked-volume
   cells. Optional profiled surface diffusion gathers the diffuse source before
   opaque SSR. The graph then traces and composites opaque SSR,
   injects/reprojects local froxel scattering, integrates the current-camera
   froxel volume and applies it, then applies analytic fog before the opaque
   transmission pyramid. Transmission and ordinary blend sample the current
   integrated volume when froxel fog is enabled. Shade transmission from deepest
   to nearest, resolve requested picking, then draw ordinary blend.
4. Reconstruct temporal Scene HDR through portable TAA, selected MetalFX or
   Vulkan FSR 3.1.
5. Meter exposure, produce/combine bloom, tonemap/FXAA and compose native UI.

Optional profiled surface diffusion uses eight scene-authored RGB distance
profiles, 32 samples and a 32 internal-pixel radius cap. Two graph-owned RGBA16F
images hold the diffuse source and composite; the existing texture system owns
the immutable 8,320-byte profile bank. The offline baker samples the matching
full-tail surface BSSRDF, including direct and photon irradiance. Native Metal
integration checks pass; [ADR-068](adr/068-profiled-surface-diffusion.md) owns the
energy allocation, geometry approximation and evidence limits.

Optional motion blur runs after reconstruction and exposure metering, before
depth of field and bloom. The separate composites preserve temporal history.
Motion blur compilation and selected native Metal output/API checks pass under [ADR-067](adr/067-post-reconstruction-motion-blur.md).
[ADR-066](adr/066-post-reconstruction-depth-of-field.md) records the accepted
budget, passing Metal checks and native Vulkan evidence limit.

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
matrices apply through the same instance contract. Cooked mesh version 17
retains source hierarchy metadata and rejects older flattened artifacts. See
[ADR-044](adr/044-shader-cross-backend-contract.md) and
[ADR-030](adr/030-offline-mesh-optimization-and-cooking.md).

Native depth-writing passes use strict less-than tests; blend/text depth reads
accept equality. Both cull with conservative affine sphere scale and retain odd
source edges during HZB reduction. Transmission compaction uses native subgroup
identity instead of assuming a workgroup invocation mapping.
Eight indirect buckets include reflection parity; direct blend preparation
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
image. Coated opaque pixels select the coat for the existing SSR ray/history;
base reflections retain probes. [ADR-062](adr/062-layered-clearcoat.md) records
the accepted scope and native evidence limits. Coated diffuse, metal and glass
share the directional energy allocation in runtime and the offline baker.

Charlie sheen uses a bounded layered response and two rectangle LTC integrals;
[ADR-063](adr/063-charlie-sheen.md) owns its 256 KiB shared table budget.
Anisotropic base GGX reflection retains authored tangent direction and adds
linear direction/strength maps, 6 MiB shared array tables and one RGBA8 axis/
strength G-buffer per target image. Runtime punctual/rectangle and offline
reflection share the material convention. Probe IBL and one-ray SSR retain
scalar-filter approximations; active anisotropy with refraction is rejected.
[ADR-064](adr/064-anisotropic-ggx-reflection.md) owns the encoding, input subset,
resource lifetime and measured fit limits.

Thin-sheet diffuse transmission partitions residual base diffuse into front
reflection and tinted direct backlighting. Material-wide strength/color use
existing visible-draw/material tables in deferred lighting and SSGI composite;
there are no new graph images. Opaque/cutout sheets exclude refraction and volume
thickness. The offline baker samples the opposite Lambert hemisphere without
changing media. [ADR-065](adr/065-thin-sheet-diffuse-transmission.md) owns the
runtime indirect-light approximation, shadow policy and evidence limits.

Optional SSGI traces one cosine-weighted ray per nearest covered half-resolution
receiver from a deterministic 256-phase Hammersley sequence. Its direct source
contains punctual/rectangle radiance and emission, excluding environment, probes,
baked diffuse, SSR, fog, and post effects. A 3×3 depth/normal bilateral raw
filter includes valid misses as zero samples before temporal filtering. SSGI selects the color/depth/identity tuple only when it matches the
motion-transform instance and submit/frame/scene tuple. Existing native queue
dependencies admit that shared predecessor while in flight; no unrelated
in-flight tuple is eligible. Unjittered motion gains the producer's
previous-minus-current raster jitter on the raw grid. FSR uses its active phase
count, MetalFX remains at eight phases, and no-TAA leaves zero jitter offsets.
Four bilinear history taps independently validate depth and identity, then resolve
RGB with bilinear × history-confidence weight. This adds nine history texture
accesses over the former single tap, with no new SSGI images or rays. When no
compatible predecessor exists, SSGI uses current radiance without waiting.
Composite applies the diffuse residual before SSR and excludes valid baked-volume
cells. It remains optional and disabled by default; [ADR-060](adr/060-screen-space-diffuse-indirect-lighting.md)
owns its storage and evidence limits.

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

Punctual lighting uses a stable 128-light table and 384-cell fragment-local
bitmask grid with exact range/cone rejection. Up to 16 ready probes contribute
fragment-space AABB weights. Directional lighting samples CSM. Point/spot shadows use a separate
16-face, 1024-squared depth pool per physical target image, with one face per
spot and six per point. Importance selection retains complete groups; excess
lights remain unshadowed. Static maps retain valid contents across frames, with
nine-tap PCF and point taps remapped across faces. Scene `casts_shadow` and the
editor's Cast shadows checkbox require a finite range. Imported glTF point/spot
lights default to casting shadows when their range is finite and positive and
their spot outer half-angle is below 90 degrees. Other imported local lights
remain unshadowed; saved editor overrides can disable shadows. Scene-authored
JSON lights remain opt-in. Light ranges,
probe bounds and GTAO do not establish arbitrary wall/furniture occlusion.
See [ADR-019](adr/019-bounded-forward-spatial-lighting.md).

Direct and environment lighting share height-correlated Smith GGX and an immutable
256×256 RG16F DFG. A per-surface energy record scales specular and reserves residual
energy for diffuse and transmission. [ADR-053](adr/053-energy-compensated-ggx.md)
owns the approximation and native evidence limits.

Scenes also support at most eight authored one-sided rectangular lights, including
disabled entries. The lighting system publishes a render-ID-sorted table; a frame
borrows it, and each backend packs 64-byte center/basis/radiance/color rows. Two
immutable 64×64 RGBA16F LTC tables occupy 64 KiB per renderer. Runtime LTC has no
area-shadow pass, while the offline baker traces rectangle visibility and transport.
[ADR-056](adr/056-rectangular-ltc-lights.md) owns authoring, lookup lifetime,
baker transport and native-evidence limits.

Scene-captured reflection probes persist as portable one-mip RGBA16F KTX2 assets.
The offline baker captures six scene-linear views and records source provenance;
normal scene loading uploads the cube and prepares SH/prefilter once. Ready local
probes work without a global environment. Local-shadow selection maximizes bounded
brightness/coverage scores with 15% incumbent preference under the 16-face budget.
Per-target maps reuse submitted static contents only while revisions and complete
light groups match; overlapping dynamic casters force their groups to redraw.
[ADR-019](adr/019-bounded-forward-spatial-lighting.md) owns these policies.

HDR source conversion, skybox and GGX prefilter use cubemaps. Diffuse lighting
uses nine GPU-resident L2 coefficients for `E/pi`, with a black sentinel and
completion-safe replacement slots. IBL bake work is not fully graph-declared.
See [ADR-016](adr/016-hdr-environment-format.md) and
[ADR-038](adr/038-sh-l2-diffuse-irradiance.md).

An enabled scene atmosphere replaces the global HDR source with a sky baked at
an authored observer altitude. Two renderer-owned RGBA16F LUTs occupy 136 KiB;
source, GGX prefilter, SH and attenuated sunlight publish together after GPU
completion. A settings revision prepares a distinct candidate while the prior
generation remains active. Camera motion does not rebake. Source RGB excludes
the direct sun disc; sky evaluation adds its alpha coverage times the same
published solar radiance that drives the direct-light and shadow policy.
[ADR-058](adr/058-revision-baked-sky-atmosphere.md) owns the model, numerical domain,
publication and offline-baker integration status.

A scene may load one immutable baked diffuse-volume texture. The 8-by-probe-count
RGBA32F texture stores seven packed `E/pi` SH vectors and room/cell metadata; the
scene owns it and completion-retires replacement or reset. A validated cell proves
all eight trilinear probes share a room before their response replaces global/probe
diffuse indirect light. It never replaces specular IBL, and existing ambient
occlusion remains active. Invalid or uncovered cells keep the global/probe diffuse
fallback. [ADR-054](adr/054-baked-diffuse-volumes.md) owns offline room detection,
artifact layout, lifetime and evidence limits.

Opaque SSR runs before transmission under
[ADR-055](adr/055-screen-space-reflections.md). Half-resolution rays retain the
current-frame depth hierarchy, full-resolution leaves and 48-decision limit.
Trace writes incoming radiance and an exact integer hit record. Temporal gathers
at most nine raw samples once for radiance and bounds (four on mirrors), then
reprojects the hit supplying the largest weighted RGB contribution using the
current and selected producer's camera and instance transforms. The virtual-hit motion delta preserves
the full-resolution receiver's offset from its half-resolution trace sample.
Coverage breaks equal-radiance ties. This prevents dark geometry from owning
history whose light comes from a neighboring lamp, without changing rejection
checks or the ray, image and texture-read budgets. Thin geometry and missing
current samples can still shimmer before stationary accumulation.

Four history taps validate receiver and reflected-instance identities, receiver
and virtual depth, selected normal and producer jitter. The previous UV ray
intersects the transported receiver plane to obtain its expected depth. A missing
hit, different traced receiver, invalid transform or rejected correspondence uses
current radiance or probes. Curved surfaces retain a tangent-plane approximation.

Full-resolution history stores incoming light. Composite applies the current
receiver's BRDF, indirect-specular GTAO, selected coat or base sheen/anisotropy
response once, while removing the exact current probe contribution. Supported
history retains the existing continuous clamp and motion-adaptive RGB retention
cap; unsupported correspondence no longer supplies a fading old reflection.

The graph owns a new half-resolution RGBA32_UINT hit image and expands geometry
history to RGBA32F (receiver/virtual depths and octahedral selected view normal)
and identity history to RGBA32_UINT (receiver/hit index-generation pairs). This
adds 98.4375 MiB at source 1280×720 with three frame slots/five history instances,
or 203.90625 MiB with eight slots/ten histories, excluding alignment and resize
overlap. Temporal adds two metadata reads, below the approved nine, and removes
its former receiver-shading reads. A 128-byte camera record borrows the exact
selected transform producer; existing waits/barriers and reader retirement remain.
`ssr_reflection` capture version 5 identifies full-resolution incoming radiance.
Native Vulkan execution and bilateral comparison remain unavailable.

SSR-enabled scenes wait 128 unchanged
submitted frames before portable TAA, FSR or MetalFX's following pass begins
128-sample static accumulation. Portable TAA caps ordinary history retention at
90% during settling, including after camera movement stops; its former stationary
99% boost could prolong reflection trails. This uses the existing TAA root word
for an explicit history mode, with unchanged image storage and texture reads.
SSR-off retention, the checked static integral, FSR and MetalFX remain unchanged.
The [cap evidence](../assets/verification/renderer-features/ssr-taa-settling-cap.txt)
shows faster convergence and increased shimmer before convergence on Metal.
Current reconstruction
continues during settling.
The selected producer's CPU metadata owns the counter; failed history/input
equality resets it. SSR history pool ownership and image count stay fixed;
[ADR-040](adr/040-metalfx-temporal-dynamic-resolution.md) owns the separate
MetalFX output-history budget.
The earlier TAA-cap evidence and the reflected-hit evidence are separated in
[ADR-055](adr/055-screen-space-reflections.md). Shader compilation and native
Metal checks do not establish Vulkan compatibility. MetalFX remains an authorized
backend-specific reconstruction mode; its eight-phase jitter and post-SDK static
accumulation are unchanged here. Its HDR capture retains private sample age in
alpha, while presentation restores opaque alpha. GPU shader validation previously
crashed in MetalTools with SSR on or off and supplied no shader-validation result.

Scenes may author analytic height fog. Frame preparation uploads one 32-byte
record per frame slot; a zero record bypasses fog. The in-place opaque/sky pass
runs after SSR and before the opaque transmission pyramid. Transmission fogs only
new local lobes over already-fogged ordered feedback, and blend retains alpha.
Fog changes invalidate normal temporal and SSR content. [ADR-057](adr/057-analytic-height-fog.md)
owns the constants, composition and Metal evidence; native Vulkan execution is
unavailable.

Froxel volumetric fog is implemented under
[ADR-059](adr/059-froxel-volumetric-fog.md). The graph reserves frame-slot-count plus two
completion-gated RGBA16F 3D local-scattering histories and one transient
RGBA16F integrated volume per frame slot. The current two-slot renderer uses
six images (10.547 MiB at 1280×720), within the approved three-slot 14.063 MiB budget. Each enabled frame uploads a 928-byte parameter
record. Metal retains its 512-byte frame root by using froxel fields at bytes 136
and 216; Vulkan's 592-byte root uses bytes 576, 584 and 588 for the parameter
address, integrated descriptor and sampler. The contract retains fields through
byte 799 and appends unjittered current view-projection and jittered inverse
raster view-projection at bytes 800 and 864. Metal native reflection, API
validation, lifecycle and numeric captures pass, as do production Vulkan SPIR-V
and host compilation checks. Native Vulkan execution and bilateral comparison
remain unavailable, so froxel fog is **UNALIGNED**.

Directional shadows default to four cascades with snapping, fit hysteresis,
per-target-image reuse and shared PCF/bias units. The nearest two cascades add
eight-sample PCSS blocker search and at most sixteen filter samples, with an
authored 0.53-degree sun diameter by default. Farther cascades retain PCF.
Static reuse requires guard
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
every pre-reset producer. A frame-input content signature plus native resource and
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
limit. Exposure, bloom and GTAO have independent frame controls. GTAO's slice
basis and horizon signs follow view reconstruction. RGBA8 outputs carry world
bent normals and visibility: global/probe diffuse uses bent sampling and
albedo-aware multi-bounce compensation, while baked volumes retain scalar AO.
A derived visibility cone occludes indirect specular, including SSR replacement.
Direct-light visibility remains shadow-owned. Direct lighting and the IBL PDF share the unclipped supported GGX lobe.
See [ADR-037](adr/037-portable-same-resolution-temporal-antialiasing.md)
and [ADR-042](adr/042-scene-linear-post-processing.md).

AgX is the default display transform; ACES fitted remains selectable. Temperature,
tint, contrast and saturation apply after exposure and before the transform, leaving
metering and scene-linear history unchanged. Neutral grading bypasses its arithmetic.
[ADR-043](adr/043-presentation-dpi-and-color-transfer.md) owns the display contract.

## Presentation and platform boundaries

Optional EDR/scRGB presentation is implemented under
[ADR-061](adr/061-extended-linear-display-output.md). A window-owned display
snapshot feeds backend output selection and a 16-byte final/UI parameter
record. Metal output and transition checks pass; native Windows/Vulkan evidence
remains unavailable. Offscreen output stays SDR.

Windows uses Per-Monitor V2 physical client pixels. SDR final shaders emit linear
RGB into sRGB attachments; UI/text authored colors decode once before linear blending.
The frame's `image_sharpness` control is finite in `[0,1]`, with zero as an exact
bypass. The sample initializes it to 0.25; zero-initialized packet callers and
harness cases default to zero. A shared, neighborhood-limited sharpening filter
operates on tone-mapped linear Scene RGB in the existing presentation draw, after
FXAA when enabled. FXAA reuses its samples and attenuates sharpening where its
subpixel blend is strongest. UI, editor recomposition, diagnostic views and
temporal histories are excluded. FSR's SDK sharpener remains disabled.
Internal Scene pixels, Scene presentation pixels and physical target/UI pixels
remain distinct. Picking and composition share viewport mapping.
See [ADR-043](adr/043-presentation-dpi-and-color-transfer.md).

Vulkan requires the explicit descriptor-buffer/device feature profile and rejects
unsupported devices; a Vulkan 1.4 version string alone is insufficient. There
is no legacy descriptor-set fallback. See
[ADR-023](adr/023-vulkan-1-4-bindless-capability-profile.md).
Requested material anisotropy uses the enabled device feature and effective
limit (up to 16); devices without it report a maximum of 1.

Metal supports explicit internal scale and MetalFX temporal reconstruction. The
sample selects dynamic MetalFX in direct and paneled modes; zero-initialized
renderer API callers use unit-scale spatial mode. A post-MetalFX pass averages
128 eligible stationary output samples after the SSR settling window. Scene,
camera, material, light, resource or viewport changes immediately restore current
MetalFX RGB and clear sample age; transparent or unreliable-motion footprints
bypass this added accumulation. The existing RGBA16F output becomes a five-instance
history image: two extra instances add 14.0625 MiB of pixel storage at 1280×720
with three frame slots, excluding native allocation rounding and resize overlap.
The pass reads current color, four existing validity texels and previous color,
then writes once; it adds no mask or depth image. This addresses stationary
shimmer, not moving MetalFX quality, and has no matched performance claim.
Vulkan spatial rendering
rejects non-unit scale and MetalFX. Vulkan FSR 3.1 accepts a fixed scale in
`[1/3, 1]`, including Native AA, with no frame generation or dynamic resolution.
It consumes raw HDR, normalized previous-UV minus current-UV motion, portable
jitter, masks and nearest transmission depth, then writes output-sized HDR before
exposure, bloom, tonemap and UI. The first slice supports finite perspective
cameras only; orthographic, infinite and reversed projections are unsupported.
The FSR mask pass puts optical contrast only in the composition mask; authored
reactivity and missing motion drive the reactive mask. TAA's full-scene and native
revision proof suppresses static composition contrast. A following compute
pass accumulates 128 stationary output samples and freezes completed pixels.
Camera or scene changes immediately restore current FSR RGB. The output uses the
five-instance graph history pool, with private sample age in alpha; fullscreen
conversion keeps final FSR alpha opaque. SDK-private accumulation remains active.
Separating optical composition from reactivity reduces motion variation around
glass. Forward/deferred specular antialiasing now shares the perceptual-to-GGX
roughness conversion on both backends; normal variance broadens squared GGX
width. Its measured benefit in the supplied Bistro view is small, and opaque
thin-edge motion remains a tuning gap. The tested camera
rotation has valid, correctly scaled opaque/transmission motion; see ADR-052 for the bounded evidence.
FSR's SDK-private resources and classic descriptors remain behind a C bridge;
the graph restores Vulkan descriptor buffers and graphics/compute offsets afterward.
Bounded native Vulkan validation passes. UI stays native after Scene reconstruction. MetalFX
motion targets the exact preceding scaler encode, with GPU event/fence ordering.
Under Metal validation, the sample explicitly uses portable TAA/spatial
diagnostics because the installed native MetalFX wrappers are incompatible. See
[ADR-039](adr/039-metal-internal-render-scale.md),
[ADR-040](adr/040-metalfx-temporal-dynamic-resolution.md) and
[ADR-052](adr/052-vulkan-fsr31-upscaling.md).

## Memory, synchronization and observability

CPU storage uses arenas for bulk lifetimes, DMemory for individual release and
pools for fixed-size churn. Borrowed views can be invalidated by capacity growth;
allocator synchronization is explicit. Vulkan driver host allocations use null
callbacks and are outside VKR CPU totals. See
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
and releases empty heaps after completed retirement. Its default 5 GiB managed
allocation cap includes heap capacity, upload/readback rings and explicit native
buffers/ICBs; opaque driver allocations remain outside that cap. Separate lifetime
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
`VKR_METAL_MEMORY_BUDGET_MB` configures the cap at startup. See
[ADR-024](adr/024-shared-bindless-gpu-cores.md) for budget and failure semantics. Its candidate preparation initializes only referenced
geometry rows in the existing completion-protected upload span; static residency
hits refresh those rows while marking resource use. Shared cores track logical
ranges, generations, submit values and retirement; physical allocations remain native. No VMA, online
defragmentation, GPU heap eviction or transient aliasing is implemented.

Slot and resource reuse require their actual last-submit completion. Vulkan
submission uses timeline values, while window presentation uses per-image
semaphores and either maintenance present fences or completed reacquire-wait
submissions. Metal maps completion to native command submission and event ordering.
Metal reserves the acquired frame's command slot while asset publication can use
another completion-protected slot. Its native configuration requires at least two
command slots; upload acquisition skips the reserved frame slot. Lifecycle changes
may wait idle; ordinary successful frames do not wait the whole device.
Capture/picking results publish asynchronously and require release.
See [ADR-009](adr/009-frame-synchronization.md) and
[ADR-014](adr/014-offscreen-present-target.md).

Both backends publish completed GPU timing/results and allocation/visibility
metrics. Unsupported timing scopes are unavailable, never zero-duration proof.
Metal compute/graphics timestamps exist; transfer timing is not supported.
The harness owns case identity, artifacts, comparison and performance authority.
After resource/bootstrap readiness it starts authored warmup at the common zero
of raster jitter and GTAO noise, with temporal history invalidated; replay version
5 includes FSR's scale-dependent jitter period in this alignment.
See [ADR-015](adr/015-metrics-module.md) and [ADR-051](adr/051-renderer-harness-and-evidence.md).

## Remaining implementation and evidence boundaries

These are limits of current code or retained acceptance, not scheduled promises:

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
  Native Metal execution and cross-backend shader acceptance remain unavailable
  for this audit. Transmission redesign, draw sorting and graph caching remain
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
  pending, failed or evicted textures; the existing MetalFX frame-rate controller
  still varies internal resolution. Failed upward tiers now require measured
  headroom before another probe, limiting repeated resizing for unchanged work.
  Recovered Scene-image allocation attempts emit one warning; terminal failures
  retain detailed errors. A separate full-target spatial check passes
  actual 1528×1074 internal rendering under the unchanged 4 GiB managed cap.
  Native Vulkan acceptance and panic causality remain unresolved; see
  [ADR-046](adr/046-editor-viewport-mapping-and-picking.md#verification-and-limits)
  for the distinct failure and memory evidence.
- Deformation/procedural/particle motion and broad animation/disocclusion coverage
  remain outside the completed rigid-motion temporal contract.
- Visibility-buffer MSAA, terrain, a general effects system, asynchronous graph
  queues and fully graph-declared IBL baking are not production features.
- Baked diffuse volumes have CPU room classification, multi-bounce and glass
  transport, portable assets, scene loading and Metal execution. Native Vulkan
  execution remains unavailable; see [ADR-054](adr/054-baked-diffuse-volumes.md).
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
  coverage, meshlets, automatic mesh LOD and shader hot reload are absent.
- Native source exists for both backends, but same-revision crossed transmission,
  visibility/packed geometry, punctual lighting, shadow-transition, tonemap,
  UI/text color/coverage/picking and mixed-DPI evidence remains incomplete.
- The Graphics Settings and nine-recipe Bakery integration is source-integrated.
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
- Near-degenerate barycentric rejection and zero interpolated tangent handedness
  still have different native edge policies, recorded in ADR-044.
- SH needs deterministic GPU projection fixtures, local-probe quality review,
  submitted-frame lifetime stress and a valid comparative performance record.
- Moving TAA/MetalFX quality, final-color baseline acceptance and authoritative
  post-effect/reconstruction performance require their own matched evidence.
  Portable Metal validation does not certify native MetalFX.
- Vulkan FSR 3.1 static, motion, Native AA, portable-TAA reference and editor-resize
  checks pass on Windows. Native Vulkan static and resize diagnostics are clean;
  performance and comprehensive temporal-quality evidence remain open.
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
