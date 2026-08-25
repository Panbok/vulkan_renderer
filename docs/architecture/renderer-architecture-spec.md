---
status: implemented
updated: 2026-08-25
authority: spec
---
# VKR Renderer — Architecture and Status Specification

**Document status:** Reviewed against the completed V7 working tree, the
accepted Metal and Windows Vulkan deferred P20 boundary, the default-on Metal
P19 compact path, the implemented P21 retirement on 2026-08-20, and the native
Windows Vulkan P21 gate passed on 2026-08-21.
Metal 4 Stages 0–5 and Vulkan V0–V6 are implemented. ADR-026 V7 is
complete under explicit owner authorization: Vulkan 1.2, its descriptor-set
shaders and manifests, the legacy-only frontend resource model, the
87-operation backend interface/adaptor, and render-pass/render-target graph
migration state are removed. `VkrRendererImpl` now selects Metal or Vulkan
once; `vulkan` unambiguously names the sole Vulkan implementation. The
complete CPU suite and surviving macOS Metal evidence pass after retirement.
Fresh native Windows Debug and Release whole-graph runs, focused offscreen and
windowed synchronization validation, and GPU-assisted validation also pass on
the target RX 6700 XT. MoltenVK still cannot execute the descriptor-buffer path.
**Scope:** Renderer architecture, implemented features, CPU/GPU memory, data
transfer, synchronization, known issues, and recommended direction.
**Audience:** Contributors and reviewers.
**Companion:** [Architecture Decision Records](adr/README.md)

This is a status snapshot, not a promise that every design document under
`docs/` describes current code. Paths and symbols are preferred over volatile
line counts.

---

## 1. Executive Summary

VKR is a C11 renderer and engine framework with two selected packet renderers:
Metal 4 on macOS and capability-gated Vulkan 1.4 with descriptor buffers on
Windows. Both consume the same versioned `VkrRenderPacket`, authored JSON graph,
generation-safe asset publications, shared GPU memory/submit/slot/capture cores,
and backend-neutral dependency schedule. API-specific pipelines, resource
realization, dynamic rendering or Metal encoders, commands, and timestamps are
owned inside the selected implementation.

The frontend owns scene-facing CPU systems and calls the selected strategy only
at coarse frame and lifecycle boundaries. Geometry, textures, samplers, and
materials publish through `VkrAssetPublisher`; there is no generic command RHI,
descriptor-instance state, frontend shader/pipeline registry, or render-pass
object model. The graph compiler remains shared because scheduling, conditions,
culling, subresource state, and dependency lowering are semantic contracts; it
does not own backend render targets or execute generic commands.

The main evidence boundary remains platform and device coverage. The complete
CPU suite and Metal runtime/validation path pass, and the post-V7 tree has been
rerun natively on an RX 6700 XT in Debug and Release. That closes the target
Windows correctness boundary, not the wider device/driver matrix. No performance
conclusion is made from the retirement deletion or the local correctness runs.

---

## 2. Project Structure

```text
lib/src/
├── application.h              Application lifecycle and packet assembly
├── core/                      Events, jobs, input, JSON, threads, ECS
├── math/                      Vector/matrix/quaternion math and frustum code
├── memory/                    Arena, DMemory, pool, allocator adapters
├── platform/                  Window and platform implementations
└── renderer/
    ├── renderer_frontend.c/h  Public orchestration and subsystem ownership
    ├── vkr_renderer.h         Public handles, descriptions, and API
    ├── vkr_render_packet.h    Versioned per-frame payload contract
    ├── vkr_render_graph.*     Graph declaration and shared schedule state
    ├── vkr_rg_compile.c       Dependencies, scheduling, culling, barriers
    ├── vkr_rg_json.c/h        JSON graph parsing and authored realization
    ├── systems/               Scene-facing renderer subsystems
    ├── resources/loaders/     Asset loaders
    ├── metal/                 Metal packet implementation
    ├── vulkan/                Vulkan 1.4 packet implementation
    └── shaders/               Shared, Metal, and Vulkan production sources
app/src/main.c                 Sample/editor application
assets/                        Materials, scenes, textures, and render graphs
tools/                         Offline utilities and renderer harness
tests/src/                     CPU-side unit and subsystem tests
docs/                          Specifications, plans, investigations, ADRs
```

The renderer frontend owns scene-facing subsystems and selects one coarse
`VkrRendererImpl` strategy. Metal and Vulkan own their packet paths,
GPU resources, pipelines, synchronization, and capture commands directly.
Public resources use generation handles or narrow opaque graph imports, and
Vulkan types stay inside `renderer/vulkan/`.

---

## 3. Frame Architecture

### 3.1 Actual frame sequence

The normal frame path is:

1. `vkr_renderer_prepare_frame()` delegates to the selected implementation,
   which proves a reusable frame slot and acquires or advances its target.
2. Async resource finalization publishes immutable asset generations through
   the selected implementation's `VkrAssetPublisher`.
3. The application creates `VkrRenderPacket` and its arrays in caller-owned
   scratch memory.
4. `vkr_renderer_submit_packet()` trusts the renderer-owned packet contract and
   delegates graph realization, recording, submission, capture, and
   presentation to the selected implementation.

This remains an ordered stateful protocol rather than a replayable command API.
Runtime graph or submission failures cancel through the selected strategy so
the acquired target and frame slot return to a valid state.

### 3.2 Packet-based submission

`VkrRenderPacket` version 18 contains frame information, globals (including
manual HDR exposure), and optional world, shadow, skybox, UI, editor, picking,
text-update, and debug payloads.
Important properties are implemented:

- optional payload pointers control pass participation;
- the frontend validates version, fixed capacities, borrowed-array presence,
  retained draw ranges, and other backend-neutral structural envelopes before
  recording; row content beyond that cold boundary remains a producer
  invariant;
- payload arrays remain caller-owned and must live until submission returns;
- draw items refer to stable renderer handles rather than Vulkan objects;
- implementations retrieve typed payloads through graph packet helpers.

The boundary should be described as **packet-based** or **value-like**, not
fully stateless:

- `prepare_frame` is a required, ordered operation;
- submission updates retained UI/text resources, publication queues, capture
  state, and metrics;
- the packet is not self-contained enough to serialize and replay without the
  retained handle registries and subsystem state;
- runtime graph or submission failures cancel the prepared frame after target
  acquisition.

### 3.3 JSON render graph

Each selected implementation parses
`assets/render_graphs/main.rendergraph.json` and uses
`vkr_rg_build_from_json()` plus `vkr_rg_compile()` to produce the active
resource declarations, pass order, culling result, and barriers for the current
dimensions and packet conditions. The implementation then realizes resources
and executes the schedule through its API-native renderer.

Implemented graph capabilities:

- declared image/buffer reads, writes, and attachments;
- dependency construction, topological ordering, and pass culling;
- conditions and `${i}` repeat expansion;
- runtime extent/format aliases;
- named executor registry owned by each selected implementation;
- graph-owned/imported resource declarations and persistent image state;
- implementation-owned CPU/GPU pass timing results;
- live/peak graph-resource statistics.

The source JSON carries mutually exclusive retained-forward and deferred world
branches, fullscreen/editor alternatives, optional picking and transmission,
the deferred visibility/G-buffer/HZB intermediates, and the Metal P19
compact-list branch. Conditions select resources, passes, and optional resource
uses before realization. Fullscreen world output feeds tonemapping while editor
world output feeds viewport composition; shadow cascades and HZB mip reductions
expand from authored repeats at build time.

#### Synchronization boundary

Barrier planning is access- and subresource-aware for declared resources:

- barriers carry `src_access`/`dst_access` and a subresource range to the
  selected implementation, whose lowerer derives API-specific stages, accesses,
  layouts, and encoder boundaries;
- a barrier is emitted when the layout changes, the access changes, **or** the
  previous access performed a write, so same-layout write→read and write→write
  hazards are represented;
- barrier state is tracked per (mip, layer), so a pass writing one cascade layer
  transitions only that layer, and contiguous subresources sharing a state
  coalesce back into a single barrier for a whole-image read;
- accesses to the same subresource within one pass are combined before its
  pre-barrier is emitted; compatible storage reads/writes become one `GENERAL`
  state, while declarations requiring incompatible layouts fail compilation;
- buffer barriers apply the same write-aware rule.

Remaining boundaries:

- all graph passes record on the graphics command buffer; compute/transfer pass
  kinds do not provide queue scheduling or ownership-transfer semantics;
- buffer barriers still cover the whole buffer rather than a byte range;
- exported images report subresource 0's layout, with a compile-time warning
  when layers disagree.

Picking, IBL, capture, and presentation are implementation-owned executions of
authored categories. Backend-internal publications and transfer work that occur
outside the authored frame schedule retain explicit implementation barriers;
the graph is authoritative for its declared frame resources, not every GPU
operation performed by an implementation.

`VKR_RG_RESOURCE_FLAG_TRANSIENT` currently means graph-owned/reusable rather
than “freed after each frame”: resources survive between realizations and are
recreated when their resolved description changes. Transient aliasing is not
implemented.

### 3.4 Shader and pipeline construction

Shader and pipeline ownership is private to each selected implementation.
Production sources live under `renderer/shaders/`: `metal/slang` and
`metal/msl` separate Metal's two source dialects, while `vulkan/slang` contains
the domain-split Vulkan packet library. `shared` is reserved for source consumed
by both production targets and does not own backend ABI or resource bindings.
Recursive SPIR-V reflection validates the Vulkan physical-storage-buffer and
packet ABI against shared host manifests. The frontend has no shader manifests,
shader system, pipeline registry, render-pass compatibility objects, or public
pipeline creation API.

Static assertions and shared `vkr_gpu_abi` manifests pin the vertex, instance,
text, and implementation-specific record layouts. Vulkan persists its
driver pipeline cache; Metal owns its archive. Both caches are implementation
details rather than frontend resource identity.

### 3.5 Scene and render extraction

`VkrWorld` is an archetype ECS with registered component layouts, entity
generation/world validation, archetypes, and compiled queries.
`VkrScene` adds transform hierarchy, visibility, renderer references, lights,
text/shapes, stable render IDs, environment IBL state, and a fixed-size array of
scene-level reflection probes.

The scene is authoritative retained state, but packets are not extracted
directly from ECS archetype arrays. `vkr_scene_handle_sync()` mirrors dirty
scene state into `VkrMeshManager`; application packet construction then scans
mesh-manager slots and submeshes. Packet construction performs a count pass and
a population pass after a capacity scan, caching visibility between the latter
two. This bridge is useful for decoupling, but it duplicates state and means ECS
locality is not yet the direct draw-collection path.

Scene resource loading is asynchronous at the CPU preparation level and is
activated once dependency/GPU finalization completes.

### 3.6 Coordinate and clip-space convention

World/view math is right-handed: `-Z` is forward, `mat4_look_at` maps a target
in front to negative view-space Z, and `mat4_perspective` maps that point to
positive clip W. Projection uses Vulkan depth `[0,1]` and inverts clip-space Y.
Perspective and renderer-facing orthographic cameras use that same clip-space
convention, and frustum extraction assumes it without matrix-shape heuristics.
Positive camera-controller forward input moves along the camera's declared
forward vector. The default sample camera starts on positive Z looking toward
`-Z`.

The P2 review found that `mat4_perspective` had left-handed signs despite its
right-handed contract. Rendering and frustum culling agreed only because both
consumed the same invalid product; camera movement, CSM extraction, local probe
selection, and future picking rays did not share that accidental meaning. The
projection signs, the controller's historical sign compensation, and
convention-specific tests now agree.

Rasterization preserves the authored counter-clockwise front-face convention
used by glTF and VKR-generated geometry. Ordinary opaque materials cull back
faces, while the skybox culls front faces to render the inside of its
outward-wound cube. IBL cube bakes use a fullscreen plane with culling and depth
disabled. The skybox pass runs before world rendering and disables depth
test/write so it contributes color without occluding geometry farther than the
finite cube mesh. Material alpha routing is resolved once per draw candidate:
`BLEND` selects the transparent world list while `CUTOUT` independently selects
the alpha-tested shadow list. Directional shadow receivers run a rotated Poisson
PCF kernel through a hardware comparison sampler, so each tap is already a
bilinear 2×2 comparison. The tap count is one of 1, 4, 9, 16, or 32, validated
before recording, and every count reads a prefix of one shared 64-entry
progressive table in `shaders/shared/shadow_kernel.slangh`. At 16 taps or more,
the default-on uniform-region policy lets a nine-tap probe resolve fully lit
and fully shadowed regions before the full kernel runs. The packet carries this
policy explicitly for matched measurement.
`vkr_shadow_light_space_origin_from_view()` converts the fitted light-view
origin into the right/up basis, including the negated X axis
introduced by `mat4_look_at`, and that origin plus `world_units_per_texel` forms
the light-space cell hash that rotates the kernel; the rotation is therefore
stable under camera motion rather than swimming with screen position.

Receiver bias has three controls, all denominated in shadow-map texels and all
converted through the owning cascade's own texel size and fitted light-space
depth span: a constant depth bias, a slope bias scaled by a clamped function of
`1 − N·L`, and a world-space normal offset applied before light projection. That
conversion is what makes one configured value mean the same world distance in
cascade 0 and cascade 3. Raster depth bias remains a separate control in backend
units. Cascades cross-fade over the last configured fraction of each cascade's
span, and shadow strength fades to zero over a configured distance before the
final split, so the shadowed range ends in a gradient rather than on a hard
edge. A cascade reused from retained contents publishes the texel size, origin,
and depth span of the fit it was *rendered* with, never the current raw fit.
Two-sided PBR shading uses
`SV_IsFrontFace` to orient geometric and tangent-space normals; it never flips
a stationary receiver according to the camera vector.

### 3.7 HDR environment, IBL, and presentation

The default world environment is the 4096×2048 Citrus Orchard Radiance HDR.
Worker-side content probing decodes finite 2:1 HDR data without vertical flip,
clamps it into binary16 range, and prepares an explicit RGBA16F payload without
using the legacy `.vkt` cache. Exact backend capability results gate both the
2D upload and cube-compatible sampled/color-attachment combinations; failure
keeps the six-face LDR cubemap active.

The runtime projects the source into a 1024², eleven-mip RGBA16F cubemap, then
bakes a 64² irradiance cubemap, a 256² full-mip GGX prefilter, and a 128² BRDF
LUT in RGBA16F. Source conversion uses explicit LOD 0 with wrapped equirect U,
and all three cube bakes reconstruct the Vulkan `+X/-X/+Y/-Y/+Z/-Z` direction
from fullscreen UV through one shared mapping. Face and roughness/mip controls
are push constants so each recorded draw retains its own values until GPU
execution. The prefilter selects source mips from the GGX light-direction PDF
and texel solid angle. Pipelines, compatible render passes, images, and
face/mip targets are prepared before the executor records. Each conversion or
convolution acquires one immutable source descriptor state; releasing it is
tagged to the frame submit serial so a later probe bake cannot update or recycle
descriptors already referenced by the command buffer. Explicit
upload/write/read barriers cover the resources that remain outside graph
authority.

Fullscreen sky/world rendering targets graph-owned `hdr_scene_color` in
RGBA16F. `VkrFrameGlobals.exposure` carries finite, non-negative manual
exposure (default `0.30`) through packet validation to
`Post.Tonemap.Fullscreen`; the pass applies it before an ACES-fitted curve into
the sRGB present target. Editor viewport display and canonical RGBA16F
scene-color capture apply the same exposure and curve. Scene environments use
a tagged cubemap/equirect source with all-or-fallback activation, while local
reflection probes remain cubemap-sourced. PBR materials use constant ambient
only when IBL is disabled, avoiding two contributions for the same environment
illumination.

### 3.8 Bistro material and spatial-lighting completion

Legacy glTF specular-glossiness materials are prepared into versioned generated
base-color and metallic-roughness textures before they enter the runtime PBR
contract. Authored dielectric F0 is retained as a uniform for factor-only
materials and an optional generated linear-RGB companion for packed
specular/glossiness textures. Direct and IBL Fresnel derive
`F90 = saturate(max(F0) * 25)`, so zero authored specular cannot regain a
camera-moving white grazing highlight. Generated namespace version 2 includes
the Khronos sub-F0 non-metal rule and companion image version 1. Runtime-written
raw mesh sidecars are retired; direct `.vkb` loads require the deterministic
version-14 packed cooked contract. Generated image and material-file publication
remains atomic and resumable.

Transmission is a distinct material and draw class. The graph renders opaque
geometry into a pre-transmission HDR image, copies it into the working scene
color while retaining the source as immutable feedback, renders transmission
with IOR, thickness, attenuation, refraction, and Fresnel, then renders ordinary
alpha blending. This prevents same-pass feedback and preserves the fullscreen
and editor output paths. Double-sided transmission faces use primitive-facing
normal orientation before refraction.

glTF punctual point, spot, and directional lights lower through the scene
loader. A stable, camera-independent scene table retains up to 128 point/spot
lights. A fixed 384-cell adaptive world grid stores one full 128-bit membership
mask per cell; finite range spheres populate it conservatively and unbounded
legacy lights use a global mask. Each fragment indexes the grid from world
position, iterates set bits, rejects zero range/cone contribution before BRDF
work, then applies exact glTF attenuation. The grid grows its cell size rather
than discarding references, so
broad material-merged submeshes do not impose a secondary light cap. The
80-byte instance ABI remains stable with its last three words reserved. Metrics
distinguish scene-table overflow and report grid cells, references, peak local
membership, and global lights. Color and intensity remain separate until the
shader applies intensity once. World draws select at most two local reflection
probes by bounding-sphere/AABB overlap; the shader computes per-fragment box
weights, normalizes overlaps, and assigns the remainder to the global
environment. Scene-environment probes retain the already-baked scene
irradiance/prefilter maps rather than duplicating them. Specular IBL applies
normal-footprint roughness filtering, specular AO, and geometric-horizon
rejection. Bistro's café volume uses an authored indoor cubemap for diffuse
irradiance rather than reusing the outdoor scene environment; its probe
specular intensity is zero because no indoor reflection capture is authored.
Diffuse irradiance samples the surface normal directly, while box projection is
restricted to specular reflection rays.

Cooked geometry's source-free boundary does not absorb scene-level glTF light
metadata. `mesh.gltf_light_source` is a separate required scene dependency:
failure to open or parse an explicitly authored path fails the scene request.
Bistro therefore loads geometry from `bistro-lights.vkb` and imports its 72
point-light instances from `bistro-lights.gltf` before activation.

---

## 4. Feature Status

| Area | Status | Current boundary |
|---|---|---|
| Selected renderer implementation | Implemented | One coarse `VkrRendererImpl` strategy selects Metal or Vulkan once; no behavior ladder or legacy adaptor remains |
| Packet submission | Implemented, partial | Versioned/validated with a real cancel path, but ordered and state-mutating |
| JSON render graph | Implemented, partial | Scheduling/culling/timing; declared binding resolution; typed direct/indirect compute descriptors; realized lifetime-aware buffers; access-, indirect-, and subresource-aware barriers; authored mip chains and per-mip views; IBL bake work remains undeclared |
| SPIR-V reflection | Implemented in Vulkan | Recursive physical-storage-buffer and packet ABI validation; no frontend shader manifests or reflection-driven pipeline system remains |
| Pipeline cache | Implemented per backend | Disk-backed Vulkan driver cache and Metal pipeline archive |
| Metrics registry and snapshot export | Implemented | Bounded typed slots, MPSC cold-event ring, triple-buffered snapshots, renderer catalog/validity, explicit GPU allocation-owner aggregates, metrics-backed HUD, atomic `--metrics-json`, and harness aggregation |
| Renderer automation harness | Implemented | Strict cases/profiles, isolated repetitions, authoritative evidence policy, aggregation, captures, comparison, autotest, baselines, and windowed/offscreen targets. Profile/snapshot parents full-content hash one transitive scene manifest, hash already-read parseable files without a second read, fan remaining files across at most eight workers, pass one digest to children, reject fingerprint drift, and verify the manifest before publication. Autotest references its primary manifest rather than rebuilding it. |
| Cascaded shadow maps | Implemented, partial quality | Four-cascade default with fit hysteresis and backend-neutral raster-bias lowering; cutout casters use the alpha-tested path, and opt-in scene-bounds Z fit clips caster bounds against each final cascade XY rectangle. Static/dynamic caster generations feed committed per-target-image retained history; P2 retains packed static candidate/instance rows per completion-protected graph slot and copies only dynamic or invalidated static ranges. Publication generation changes revalidate every slot. Guard-contained static cascades omit their authored graph passes, while dynamic overlap, incomplete publication, invalid retained contents, or signature drift fail closed. P0 CPU scopes and P3B reuse/force counters ship on both backends. P5 adds a bounded lowest-margin proactive-refresh scheduler and defaults its budget to zero. P4 was declined after its Metal predictor deferred zero candidates across 600 moving-camera frames, preserving the exact-gated one-phase topology. P6 implements graph-declared occupied-depth SDSM on Metal and Vulkan with completion-gated asynchronous feedback, source metadata, smoothing/fallback metrics, and a harness opt-in; fixed splits remain the default because the matched local Metal control measured +1.310 ms/frame of combined reduction and realized cascade work. Native RX 6700 XT synchronization validation covers active Vulkan feedback at three target images; no Vulkan timing claim is made. Receiver quality (P7) ships: a rotated Poisson PCF kernel through a comparison sampler at 1/4/9/16/32 taps from one shared progressive table, a case-selectable nine-tap uniform-region early out at 16 taps or more, texel-denominated constant/slope/normal-offset bias converted through each cascade's own texel size and fitted depth span, cascade cross-fade, and max-distance fade. Reused cascades publish the fit they were rendered with. Metal tap, early-out, split-lambda, and map-size experiments retained the current defaults; the shadow rewrite spec records their aggregates and evidence limits. Shadow distance remains a separate quality experiment |
| PBR materials | Implemented, evolving | Metallic-roughness and texture slots plus prepared, cached specular-glossiness lowering with retained dielectric F0/F90 response; transmission adds IOR, volume, attenuation, and scene-color refraction while clearcoat and sheen remain absent |
| IBL | Implemented, partial integration | HDR/cubemap sources, prepared RGBA16F bakes, global environment, and two fragment-weighted local probes per draw ship; bake work remains undeclared to the graph and explicitly barriered |
| glTF and scene loading | Implemented | CPU async pipeline; nested texture URIs and sidecars resolve without flattening; `EXT_meshopt_compression` buffer views decode before accessor reads; UVs lower once to VKR convention; point, spot, and directional punctual lights import through the scene transform into a stable 128-light table with a fragment-local 384-cell bitmask grid. Cooked mesh entities may name `mesh.gltf_light_source` explicitly so geometry remains `.vkb` while scene-level light metadata comes from glTF. Required mesh dependency or GPU-publication failures fail the scene request instead of activating a meshless scene; frame-path uploads measured non-blocking |
| Offline mesh cooking and packed geometry | Implemented | `vkr_mesh_cooker` uses pinned meshoptimizer v1.2 to atomically emit deterministic version-14 `.vkb` artifacts with per-range cache/fetch optimization, 32-byte packed static vertices, explicit quantization budgets, SHA-256 dependency/settings provenance, and CRC32 metadata/stream checksums. Worker decode performs no authoring-source I/O; mandatory runtime optimization applies to every OBJ/glTF/GLB source load. Production cook scripts/references, lifecycle stress coverage, byte/locality metrics, and branchless Metal/Vulkan packed publication ship. Indices remain 32-bit pending an explicitly partitioned draw/resolve ABI and matched evidence. |
| Transmission | Implemented, bounded deferred paths | Graph-declared opaque, HDR feedback-copy, transmission, and ordinary-blend stages. Both backends peel and composite four ordered transmissive surfaces and publish completion-gated per-layer coverage. Metal defaults to the eight-pass compact-scan/indirect-shade P19 path; `VKR_TRANSMISSION_COMPACT_DISABLED=1` selects its focused full-screen diagnostic rollback. Vulkan retains full-screen shading. No order-independent or unbounded deep compositing is claimed |
| KTX2/UASTC textures | Implemented | BC7/BC5, ASTC, ETC2, EAC RG11, and RGBA32 paths; 2D arrays, cubemaps, and cubemap arrays lower to native Metal/Vulkan view types, while a Metal compute diagnostic samples nonzero array/cube-array indices. Runtime resolution is strict KTX2 by default; explicit test/development flags retain source/legacy coverage, and the packer replaces legacy outputs regardless of timestamp. Material streaming admits eight requests, defaults to uncapped full residency, automatically applies a 90/80/75% heap-budget pressure hysteresis every 60 frames, honors explicit overrides, uniquely accounts shared textures, and completion-retires only last references. Metal batches up to 64 copies into one 32 MiB upload command; Vulkan records into its active frame command buffer. |
| Editor viewport and picking | Partial | Picking is fully declared in the render graph and runs; readback is usually deferred but ring wrap can block. Both packet implementations copy `editor_enabled` into the graph frame, so the authored editor branch is reachable. Both implementations still pin viewport extent to window size; a true offscreen editor viewport remains absent. See §8 P1 item 15 |
| UI system | Absent | `VkrUiSystem` is 16 corner-anchored text slots. No rectangle primitive, layout engine, hit testing, clipping, or UI input model. `VkrUiPassPayload.draws` is plumbed end to end but always submitted empty. Design in [ui-architecture-spec.md](../ui/ui-architecture-spec.md), rationale in [ADR-027](adr/027-immediate-mode-grid-ui.md); both are `proposed` |
| Text | Implemented | Bitmap, MTSDF, system-font, UI and world text paths publish packet-native resources; the dedicated Metal harness fixture remains deterministic |
| CPU frustum culling | Feature-local only | Ordinary alpha blend remains conservatively camera-culled and back-to-front sorted. Opaque, cutout, transmission, and shadow visibility are GPU-classified from candidate streams |
| Draw batching | GPU-owned world submission | Opaque, cutout, transmission, and shadow commands are compacted into backend-native indirect buckets. Ordinary blend remains ordered and direct |
| Multi-draw indirect | Implementation-owned | Metal executes GPU-encoded ICB ranges and Vulkan executes fixed-partition indirect-count draws; there is no generic CPU indirect subsystem or direct world fallback |
| Compute dispatch | Implemented; production Metal and Vulkan deferred kernels | Typed executors carry validated direct or indirect launch descriptors without per-frame name lookup. Metal P4/P8/P10/P12/P14 uses graph-declared classify, prefix, ICB encode, G-buffer resolve, deferred-lighting, fused-transmission, and HZB-reduction kernels. Vulkan P5/P9/P11/P13/P15 provides classify/prefix/encode, G-buffer, lighting, transmission, coverage, and HZB kernels plus indirect-count raster, with its opaque and transmission shading sharing the forward fragment shader's lighting helpers and surface reconstruction; P20 owner evidence is accepted |
| GPU timing | Implemented with backend-specific scope | Vulkan retains per-pass query timing. Metal compute and graphics passes use precise encoder-scope timestamps with completion-owned query storage; transfer and zero-work scopes remain explicitly unsupported. Metal also publishes exact `gpu.submission` start-to-end latency from `MTL4CommitFeedback`. The harness associates both asynchronous result paths with their source frames and drains after the measured window |
| Device-memory suballocation | Implemented | Vulkan uses keyed DEVICE, UPLOAD, and READBACK pools backed by `vkr_gpu_memory`; Metal uses the same range/submit cores through its placement adapter. Logical and physical totals, peaks, retirement, failure classes, and capacity lower into renderer metrics. |
| Bindless resource model | Implemented | Metal 4 and Vulkan 1.4 use GPU-addressed buffers, backend-native texture/sampler rows, completion-gated publication/retirement, authored graph lowering, and shared memory/submit/slot/capture cores. Immutable GPU material rows carry PBR parameters; each non-empty indexed packet pass publishes one 448-byte Metal frame root (464-byte Vulkan) and each retained draw references geometry/visible tables through a reflected 48-byte draw root. Shared flags give lighting, IBL readiness, and transmission identical shader semantics. Packet version 18 carries the bounded opaque/cutout/transmission GPU-candidate streams, static/dynamic/publication generations, packetized shadow-debug and independent pass/submission timing requests, feature-local blend/text payloads, no CPU opaque/transmission/shadow draw lists, the backend-neutral per-cascade and shared receiver-quality blocks, and SDSM enable/source-scene metadata. Corrected Metal pixels are owner-accepted, though no replacement golden generation is accepted; Vulkan is the sole Vulkan renderer after V7. Native Apple M1 Pro reflection, focused Metal API/GPU validation, and exact text/picking captures pass for the changed root ABI; the bindless-renderer audit records the evidence. |
| Deferred visibility-buffer migration | Implemented through P21 | Both backends execute one topology: bounded GPU candidate classification and indirect submission for camera/cascade views; opaque visibility, G-buffer resolve, HDR lighting, HZB, picking, and four-layer transmission; completion-gated coverage and diagnostics. P21 removed the selector, legacy graph branch, CPU opaque/transmission/shadow draw construction, fallback routes, backend executors/pipelines/shaders, and dual-path metrics/tests. Packet version 18 rejects invalid, zero-generation, or over-capacity input before recording and carries completion-safe candidate publication generations. Ordinary blend, world/UI text, UI, post, and their picking coverage remain narrow feature passes. The accepted P20 Windows evidence and visual threshold remain the migration oracle. The native Windows Vulkan P21 gate passed on 2026-08-21 after repairing a retirement defect that made the bounded geometry/material publication boundary fail command recording instead of omitting the affected candidates for the frame; a malformed submesh index is still an explicit pre-recording error. P19 is Metal-only and default-on after its scan/finalize consolidation; Vulkan retains full-screen transmission. |
| Vulkan V1–V4 migration | Complete for RX 6700 XT | V1 characterization and V2's selected strategy are complete on macOS and Windows. V3 extracted the memory, submit-ring, and shared ABI cores alongside their production Vulkan callers and passes native window resize with reacquisition and retired-swapchain completion proof. V4 extracted the slot table, added completion-gated asset publication, and moved geometry, staging, images, startup buffers, and readback into keyed dynamic pools with complete logical/physical metrics. Prepared and writable initialization records before the next frame draw, staging retirement uses that submit value, publication dirty ranges flush once per backing buffer, and logical totals return to baseline. MoltenVK cannot execute the descriptor-buffer path. ADR-024's required cross-platform extraction witnesses now pass. |
| Vulkan V5–V7 | Implemented; post-V7 target rerun passes | V5 lowers the authored graph to synchronization2/dynamic rendering and implements all packet pass/capture/timing categories. V6 completed selection, cache, lifecycle, metrics, and native RX 6700 XT validation. V7 removed the Vulkan 1.2 path, temporary selector, shaders/manifests, legacy frontend systems, interface/adaptor, and graph residue. CPU and Metal gates pass after deletion; fresh RX 6700 XT Debug/Release whole-graph, synchronization-validation, and GPU-assisted witnesses pass. |
| HDR/tonemap/post chain | Implemented, initial | RGBA16F fullscreen/editor scene color, packet-carried manual exposure (default `0.30`), ACES-fitted tonemap, and exposure-equivalent canonical HDR capture; automatic exposure and additional post effects are absent |
| Shader hot reload | Absent | Build-time shader compilation only |

---

## 5. CPU Memory

### 5.1 Allocator model

`VkrAllocator` adapts three allocation backends:

| Backend | Lifetime model | Typical use |
|---|---|---|
| `Arena` | Bump allocation and bulk reset/destroy | Scratch or data sharing one lifetime |
| `VkrDMemory` | Reserved/committed virtual memory with individual free | Registries, keys, reloadable objects |
| `VkrPool` | Fixed-size slots | Homogeneous objects |

`VkrArenaPool` is a separate thread-safe pool of fixed-size chunks used to make
asset-loader scratch arenas. It is not a fourth `VkrAllocator` backend.

The interface provides optional scopes, aligned operations, tagged local/global
accounting, and `_ts` wrappers that synchronize through a caller-supplied
mutex. Allocator objects are not intrinsically thread-safe. Global counters are
atomic.

Arena bulk destruction must be paired with
`vkr_allocator_release_global_accounting()` when global tag totals must remain
accurate. The scene runtime does this before destroying its scene arena. The
repository guidelines correctly require freeable allocation for removable hash
keys and acquire/release symmetry for renderer handles.

These policies reduce scene-reload growth; they do not prove that all reload
paths are leak-free. Repeated-load measurements and handle counters remain the
required validation.

### 5.2 Vulkan host allocator

Vulkan currently passes `NULL` `VkAllocationCallbacks`, so driver host
allocations use the Vulkan implementation's default allocator and are not part
of VKR's CPU allocator totals. GPU device-memory placement remains explicitly
tracked through `vkr_gpu_memory`.

---

## 6. GPU Memory

There is no VMA. Vulkan suballocates buffers and optimal-tiling images
from keyed DEVICE, UPLOAD, and READBACK blocks through `vkr_gpu_memory`. Pool
keys include resource kind, exact memory type, and device-address requirement;
host-visible blocks are persistently mapped. Required dedicated allocations
bypass the range core while retaining the same logical and physical metrics.

The adapter publishes physical-allocation and logical-suballocation totals,
peaks, owner classes, retirement, free space, largest range, bounded failure
classes, and capacity. Metal uses the same shared range and submit cores behind
its placement-heap adapter. Neither implementation currently provides online
defragmentation, eviction, or transient image/buffer aliasing.

---

## 7. CPU↔GPU Communication and Synchronization

### 7.1 Per-frame mapped data

Each selected implementation owns bounded per-frame upload and instance storage
indexed by a completion-protected frame slot. Packet validation and metrics
report overflow rather than silently wrapping. Vulkan material and
texture references are generation-safe indices into descriptor buffers and
slot tables; Metal uses native resource identifiers in its material rows.
There are no frontend global UBOs, descriptor sets, instance-state pool, or
generic indirect-command stream.

### 7.2 Resource preparation and upload

`VkrResourceLoader` separates worker-thread `prepare_async` from render-thread
`finalize_async`. The resource pump has count/op/byte budgets and guarantees
forward progress by permitting the first oversized request.

Finalization publishes decoded assets through `VkrAssetPublisher`. Vulkan
records prepared and writable initialization into the next frame command
buffer, batches dirty-range flushes, and retires staging at that submit value.
Metal texture finalization packs up to 64 payloads into one aligned 32 MiB
upload-ring slice and one command buffer; oversized payloads receive a dedicated
slice. Batch completion is proven before frame recording, and a begin/end
failure aborts preparation rather than exposing an unsubmitted publication.
Replacement is publish-new-then-retire-old, and generation slots are not reused
until their recorded completion value is proven.

Material finalization does not make scene activation depend on texture
residency. It publishes default-textured rows, queues texture paths, and admits
at most eight texture resource requests at once. READY textures republish their
material row. Normal loading has no residency cap, so a scene can reach full
texture residency. Backend heap telemetry is sampled every 60 frames; crossing
90% applies a texture limit targeting 80% total use, and falling below 75%
clears it. `VKR_TEXTURE_STREAM_BUDGET_MB` and the runtime setter override that
automatic policy. Residency counts each shared GPU texture once, pins incoming
shared handles before choosing victims, republishes semantic defaults, and
completion-retires a texture only after its last resident slot leaves.
Continuously demanded evictions remain stable; reload requires an
unused-to-used transition. Pending, active, resident, evicted, byte-budget,
applied, failure, and pressure counters are metrics.

### 7.3 Readback

Picking and harness capture are asynchronous packet requests. Implementations
copy into bounded readback storage and publish results only after the associated
submit completes. `vkr_capture_ring` owns request state and explicit release;
capacity exhaustion returns a busy error rather than overwriting live results.

### 7.4 Frame synchronization

The shared submit ring assigns monotonic completion values to bounded frame
slots and retirement records. Vulkan signals a timeline semaphore for
GPU completion. Windowed presentation uses per-image render-complete semaphores;
optional maintenance1 fences provide explicit present completion, while the
portable path uses a completed submit that consumed the reacquired image's
acquire semaphore as proof. Metal maps the same completion contract to command
buffer completion and residency ownership.

Normal successful frames do not wait the whole device. Explicit idle waits are
reserved for lifecycle operations such as shutdown or target recreation.
Command-slot reuse can still wait when CPU submission outruns the bounded ring,
and that wait is published as a metric.

---

## 8. Prioritized Issues

Current priorities after V7 are:

1. Decide whether to publish new Metal and Vulkan snapshot baselines. Corrected
   pixels and the retained-forward oracle are owner-accepted, but no historical
   generation was replaced implicitly.
2. ~~Complete bounded Bistro asset startup after persistent transcode caching.~~
   **Done for scene activation and incremental material residency.** Generated
   derivatives have `.vkt` siblings, target-native transcodes persist across
   processes, Metal batches texture copies, and materials publish with defaults
   before an eight-request texture residency window begins. A Release Metal
   Bistro run reached `boot.scene=1.141 s`, issued six world draws, and sampled
   777 pending / 8 in-flight streams while completed textures advanced from 14
   to 18. Its raw sample digest is
   `sha256:a2cfd93d61f049cc4be9b0b0a737ab079d25715d2eed34905129de0f6659ddbf`.
   The profile wrapper itself remained unsuitable for a short wall-clock gate:
   after rendering completed, both child and parent content-manifest passes
   hashed the full multi-gigabyte dependency set. That harness provenance cost
   is not renderer startup and remains separate tooling work.
3. Establish matched Release evidence before making any performance claim about
   the two surviving implementations or the V7 deletion.
3b. **Metal compute and graphics pass timing is repaired; transfer timing is
   explicitly unsupported.** Both timestamps were historically written with
   `[MTL4CommandBuffer writeTimestampIntoHeap:atIndex:]`, outside the encoder.
   Under Metal 4's overlapping encoders that interval measured command-stream
   progress rather than the named pass. Two repetitions of one binary with
   **bit-identical work volume** (84,645 indirect commands, 65,269 visible) and
   `frame.wall` 4.4% apart attributed their GPU time 45% differently;
   `Lighting.Deferred` alone read 6.2003 ms and 0.0125 ms. Which pass collapsed
   moved between runs, and the defect predated receiver-quality work.

   Requested Metal compute and graphics passes now write precise timestamps on
   their owning `MTL4ComputeCommandEncoder` or `MTL4RenderCommandEncoder`.
   Every command slot owns one fixed timestamp heap. The heap is invalidated
   before recording, retained through shared-event completion, and resolved
   only when that slot completes. A sample is valid only when `begin > 0` and
   `end > begin`; no command-buffer-scope fallback or coverage heuristic
   remains. Vulkan per-pass query timing is unchanged.

   Transfer passes remain unavailable. On the tested Metal 4 device and API,
   precise compute-encoder timestamps around one 4 MiB copy, 64 consecutive
   4 KiB copies, and the transfer third of a mixed lane all resolved equal
   begin/end values. Passes that submit no timed dispatch or fragment work can
   produce the same result. These rows publish
   `unsupported_timestamp_scope`, never zero milliseconds. The harness treats
   that explicit classification as complete availability evidence while
   retaining an invalid timing sample, so it cannot support a duration claim.

   Metal also publishes a separate exact `gpu.submission` duration from
   `MTL4CommitFeedback.GPUStartTime` and `GPUEndTime`. Sixteen fixed feedback
   records outlive command-slot reuse. Each owns a precreated commit-options
   object plus feedback and trailing-ack blocks; because feedback handlers are
   one-shot, the same retained block is registered again only when its record
   is recycled. One process-lifetime serial queue runs feedback and publishes
   readiness from the trailing block, proving the callback has returned before
   options reuse. Atomic feedback/result halves prevent either completion order
   from exposing a partial result. The harness maps each asynchronous
   completion back to its source frame and drains only after the measured
   window. `gpu.submission` is submission start-to-end latency, not a pass sum
   or GPU-busy counter; overlapping submissions can make its total exceed wall
   time. Pass and submission timing flags remain independent comparison inputs.

   The standalone `vkr_metal_timestamp_diagnostic` now covers real compute and
   render pipelines, cross- and intra-encoder barriers, one or two residency
   sets, buffer-backed argument tables, direct/untracked/placement resources,
   GPU-authored ICBs, GPU-written indirect ranges, and the production indexed
   ICB contract. That indexed contract uses `DrawIndexed`, inherited pipeline
   state, explicit vertex/fragment root bindings, and a private index buffer.
   Its full mixed lane resolved 720/720 strict intervals across 24 encoders,
   30 iterations, and three rotating slots.

   An earlier production experiment stalled GPU completion, including a
   compute-only variant. That stall does not reproduce on the current renderer:
   six independent cold Sponza processes completed the full production graph
   with strict compute/graphics intervals and no command-slot timeout. Five
   steady `Lighting.Deferred.Fullscreen` process means were 0.5937 to 0.6005 ms;
   the first process averaged 0.6423 ms because one of its three frames reached
   0.7329 ms. These local dirty observations prove completion and attribution
   mechanics, not performance. A final default-path run passed with 0.5933 to
   0.5967 ms across three frames, and focused Metal API validation reported no
   diagnostic beyond its enablement notice. No causal claim is made about why
   the former stall disappeared. Both used
   `build_release/tools/vkr_harness profile --case
   tools/cases/smoke/sponza_offscreen.case.json --profile
   tools/profiles/local-offscreen-gpu-single.json`; the default report digest
   was
   `sha256:1a515b85b2713c799aa4c5d1f98a9d30eaea374c3d78b2189088da9843d4e9ba`
   and the `MTL_DEBUG_LAYER=1` report digest was
   `sha256:4a729855a07b6bdcb3815a20ee094faeb010c07e2ca6d8744cccd28c9ce755c9`.
4. Establish a direct same-surface temporal texture-attachment oracle. The
   reported Vulkan-only inversion/swimming defect was a visibility-resolve
   framebuffer-to-NDC Y mismatch: the positive-height Vulkan viewport and
   already Y-inverted projection were flipped a second time during attribute
   reconstruction. A moving-camera Vulkan snapshot now exercises resolved
   diffuse, barycentric/LOD, and final-colour endpoints at the reported Bistro
   sign. Still images do not measure attachment error directly, so retain a
   focused temporal metric as regression automation even though this concrete
   mapping defect is closed.
5. Broaden native Vulkan validation beyond the target RX 6700 XT. §10 records
   what was closed on 2026-08-21 (two-/four-image WSI-free targets, picking, and
   a windowed resize round trip under validation) and which parts remain
   blocked by absent capability: other GPUs, windowed image-count selection,
   queue-family layouts, minimize/cancel, and injected acquire/fence/submit/
   present failures.
6. Treat Linux as unsupported until its platform integration and native evidence
   exist.

The dated items below are a historical closed-work log. Symbols belonging to
the Vulkan 1.2 backend, generic graph executor, shader/pipeline registry,
instance buffer, or indirect-draw system were removed by ADR-026 and are not
current architecture or open work.

### P0 — Correctness and error contracts — **resolved 2026-07-31**

All five landed together; they are listed here as the record of what changed.
Barrier and frame-path behaviour passed a validation-layer startup/steady-frame
smoke run with a three-image swapchain. The broader scenario matrix remains a
required gate — see §10.

1. ~~**Index frame streams by the frame-in-flight slot.**~~ Done.
   `frame_in_flight_index_get` / `frame_in_flight_count_get` were added to
   `VkrRendererBackendInterface`, and `VkrInstanceBufferPool` /
   `VkrIndirectDrawSystem` are now indexed by the fence-protected slot instead
   of `image_index % 3`. `vkr_renderer_initialize` fails loudly if the backend
   reports more in-flight frames than the pools hold. Two further defects were
   found and fixed in the same area: `current_frame` was advanced twice per
   frame (`renderer_vulkan_end_frame` *and* the tail of
   `vulkan_swapchain_present`), which pinned the slot at 0 forever on a
   two-image swapchain; and `images_in_flight` kept pointers into the
   `in_flight_fences` array across a swapchain recreate that destroyed it.
2. ~~**Propagate backend failures.**~~ Done. `VKR_RENDERER_ERROR_FRAME_SKIPPED`
   and `VKR_RENDERER_ERROR_SUBMISSION_FAILED` were added;
   `vulkan_swapchain_acquire_next_image` and `vulkan_swapchain_present` return a
   tri-state `VulkanSwapchainResult` so a recreate-and-skip is no longer
   indistinguishable from device loss. All seven false-success sites now return
   real codes. `renderer_vulkan_begin_frame` has a single cleanup path, and a
   frame that consumed an acquire without submitting sets
   `frame_recovery_required`, which forces a device idle and swapchain recreate
   before the next acquire. The acquire path no longer retries with a semaphore
   that the recreate just destroyed. Swapchain-sized arrays now use the actual
   image count returned by `vkGetSwapchainImagesKHR`, not the requested
   `minImageCount`; queue-family indices also remain alive through
   `vkCreateSwapchainKHR`. A terminal recreation failure marks the backend
   unusable, so the void resize callback cannot leave later frames touching
   partially rebuilt WSI state; the next frame surfaces `DEVICE_ERROR`.
3. ~~**Add an explicit cancel path.**~~ Done. Packet validation moved into a
   side-effect-free `vkr_renderer_validate_packet`, collapsing 29 duplicated
   exits into one `goto cancel`. `vkr_renderer_cancel_frame` and the backend
   `cancel_frame` entry reset the primary command buffer, discarding any passes,
   barriers, timestamps, and readbacks recorded before the failure. They then
   record and submit only an `UNDEFINED`→`PRESENT_SRC_KHR` transition, consuming
   the acquire semaphore without presenting a partial frame. Newly-started
   picking readbacks are rolled back in both the backend ring and retained
   picking state.
4. ~~**Make graph execution fallible.**~~ Done. `vkr_rg_execute` returns
   `VkrRendererError`; barrier application and begin/end render pass are
   checked and abort at the first failure; `VkrRgPassContext.error` lets an
   executor report a recording failure without changing the executor signature.
   Missing runtime image/buffer handles and missing graphics render targets are
   errors rather than silently skipped work. `vkr_renderer_submit_packet`
   cancels the frame on graph failure rather than presenting a partially
   recorded one, and cancellation failures replace the original result as the
   actionable lifecycle error.
5. ~~**Complete graph synchronization.**~~ Done for declared resources.
   `VkrRgImageAccessFlags` is now an alias of a renderer-wide
   `VkrImageAccessFlags`, and the backend's `image_barrier` entry derives
   `VkAccessFlags` and `VkPipelineStageFlags` from those accesses — replacing
   the 21-entry old/new layout table, which could not express a same-layout
   hazard and always covered the whole image. Barrier state is tracked per
   (mip, layer); a barrier is emitted when the layout changes, the access
   changes, **or** the previous access wrote. Contiguous subresources with
   identical state coalesce into one barrier. Same-pass accesses are aggregated
   before barrier emission: compatible storage read/write flags are unioned,
   while incompatible layouts are rejected. Image storage usage is now exposed
   through `VKR_TEXTURE_USAGE_STORAGE`, JSON `"STORAGE"`, and
   `VK_IMAGE_USAGE_STORAGE_BIT`, so storage declarations are validated against
   a real image-creation capability. Concretely, the four-cascade
   `shadow_map` now emits one per-layer barrier per cascade — previously
   cascades 1–3 emitted nothing at all — and a single merged barrier for the
   whole-image `World.*` read. Each cascade, including layer 0, also receives an
   exact one-layer framebuffer view; using the default whole-array view for
   layer 0 made Vulkan require untouched layers to be attachment-optimal and
   was caught by the validation smoke run. The layout-pair table remains in
   `vulkan_image.c` for the ~20 upload, mipmap, and copy call sites that
   legitimately think in layout pairs and run outside the graph.

   Graph-owned contents can now survive across frames. `RETAINED` (ADR-029)
   seeds a resource's barrier planning from the last successfully submitted
   state instead of `UNDEFINED`, tracked per physical instance and per
   subresource by the selected implementation, and committed only after a submit
   is proven. Reading a retained subresource with no valid contents is a compile
   error rather than the warning `PERSISTENT` produces — `PERSISTENT` only
   suppresses that diagnostic and preserves nothing. `shadow_map` is declared
   `RETAINED, PER_IMAGE, RESIZABLE`. Each cascade pass selected by the committed
   per-image history clears and rewrites its layer, while guard-contained
   reusable layers omit their authored passes and preserve prior contents.
   Dynamic-caster overlap, incomplete publication, invalid retained contents,
   or signature drift forces a rewrite. The Vulkan half is implemented and
   CPU-tested but has never executed, because Vulkan initialization is
   Windows-only.

   **Residual gap:** this is correct synchronization for *declared* resources.
   Picking is now declared (P1 item 6), but IBL baking still records nested GPU
   work on resources the graph cannot see.

### ~~P0 — Descriptor sampler-limit compatibility~~ Resolved 2026-08-05

PBR still exposes 17 independently bound sampled images, but reflection and
the shader manifest now separate image slots from sampler slots. Irradiance
probe B/C share probe A's sampler, as do prefilter probe B/C, because those
generated maps have identical filtering/addressing semantics. Authored material
textures retain their independent sampler state. The resulting fragment layout
uses 13 samplers, below the Apple M1 Pro / MoltenVK limit of 16. Reflection tests
pin the 17-image/13-sampler contract, and exact Bistro validation replay
`20260805T101216.174Z-00c0d6` contains no
`VUID-VkPipelineLayoutCreateInfo-descriptorType-03016` diagnostic.

### P1 — Architectural completion

6. **Bring picking and IBL GPU work into the graph.** Picking: **done
   2026-07-31.** `picking_color` and `picking_depth` are graph resources sized
   from the viewport and gated by a new `picking_pending` condition, so a
   non-picking frame allocates nothing for them (measured: 197 → 195 live
   device allocations at scene load). `Picking.Request` is a declared graphics
   pass in the `PICKING` domain; a new `Picking.Readback` pass declares the
   `TRANSFER_SRC` read that produces the copy-source layout, which previously
   came from the picking render pass's `finalLayout` and was invisible to the
   graph. The picking system no longer owns the texture, depth, or target — it
   keeps its render pass purely so pipelines have something compatible to be
   created against, plus the readback ring and the pick-coordinate bounds. The
   dead `vkr_picking_render` path was removed.

   IBL: **not declared, and declaring it would have been theatre.** Its outputs
   are written by the bake's own render passes (`finalLayout` =
   `SHADER_READ_ONLY`) and sampled through material descriptors; no other graph
   pass touches them. Declaring them would add a scheduling edge that the
   declaration order already provides and a barrier that the read-after-read
   rule correctly skips — zero emitted barriers, for a substantial amount of
   dynamic-handle import plumbing.

   **What the investigation did find is a real hazard, now fixed.** Render
   passes are created with a subpass→EXTERNAL dependency of
   `dstStageMask = BOTTOM_OF_PIPE`, `dstAccessMask = 0` — an execution-only
   dependency that performs no visibility operation. A `finalLayout` transition
   is not a visibility operation either. Graph-declared resources are covered
   because the graph emits explicit access-carrying barriers (P0 item 5), but
   the IBL bake's outputs are produced inside an executor and are invisible to
   it, so nothing guaranteed a later fragment shader saw those writes.
   `vkr_world_resources_bake_cubemap` now emits an explicit
   `COLOR_ATTACHMENT → SAMPLED` barrier on each baked cubemap. It is a
   same-layout write→read barrier — precisely the hazard class the P0 barrier
   work was built to express, and one the old layout-pair model could not have
   represented.

   The genuine remaining gap is unchanged: the bake records prepared render
   targets and passes the graph cannot see. Pipelines, images, descriptors, and
   face/mip targets are no longer lazily created in the executor, but closing
   graph authority still means importing or declaring those persistent
   resources and passes.
7. ~~**Reflect and validate uniform members.**~~ **Done 2026-07-31.**
   `vulkan_reflection_collect_uniform_blocks` populates what was previously
   hardcoded to zero, and `vulkan_shader_validate_uniform_layout` cross-checks
   each manifest declaration against the correct reflected frame/draw block at
   shader creation. Name, offset, exact size, scalar/vector/matrix shape, array
   count/stride, and matrix stride must agree; matching `(set,binding)` blocks
   across shader stages must have identical member layouts. Slang's generated
   matrix/array storage wrappers are normalized into those traits. Samplers are
   skipped (addressed by `location`, not offset); a manifest entry with no
   reflected member is an error, while a reflected member the manifest never
   declares is permitted.

   **This found a shipped defect.** `default.ui` and `default.viewport_display`
   declared `view` before `projection` in their Slang UBOs while their manifests
   declared the opposite order, so the host wrote the projection matrix into the
   shader's `view` slot and vice versa. It was invisible only because the UI view
   matrix is identity, which makes the two interchangeable. Both shaders were
   reordered to the projection-then-view convention every other shader uses.
   `tests/src/reflection_pipeline_test.c` now sweeps all 13 shipped
   `.shadercfg`/`.spv` pairs through the shipped validator, so drift fails a
   test rather than a launch.
8. **Make uploads genuinely asynchronous.** Two of three parts are **already
   satisfied, now measured** rather than assumed:

   - *No infinite waits in frame finalization.* `vkr_resource_system_pump`
     drives `finalize_async` on the render thread with `frame_active` set, so
     the wait counters' guard can fire. A full Sponza load with
     `VKR_ASSERT_NO_UPLOAD_WAITS=1` reports
     `fence=0 queue_idle=0 device_idle=0`, with zero "refusing blocking
     single-use submit" logs — the uploads take the deferred in-frame recording
     path rather than being skipped, so the zero is meaningful.
   - *Staging retained until completion.* Deferred destroy is gated on
     `submit_serial` against `completed_submit_serial`, which only advances
     after a frame-slot fence wait proves GPU completion. Staging enqueued
     during a frame is stamped `submit_serial + 1` — the submission that will
     contain its copy. On queue saturation it deliberately leaks rather than
     free early.
   - *Timeline semaphores* remain unimplemented, and were out of the agreed
     scope for this pass.

   Uploads outside an active frame (bootstrap, single-use submits with no frame
   recording) still block. That costs scene **load** time, not frame time, and
   quantifying it needs a Release measurement rather than the Debug figure in
   the GPU memory baseline.
9. ~~**Fix KTX2 normal-map capability fallback.**~~ **Done 2026-07-31.**
   The fix belonged in the selector, not the mapper: libktx has no uncompressed
   two-channel transcode target, so `R8G8_UNORM` could never be reachable.
   Added `VKR_TEXTURE_FORMAT_EAC_R11G11_UNORM` with its own capability probe
   (queried separately from ETC2 RGBA rather than assumed to follow it), placed
   it in the `NORMAL_RG` ladder below BC5 and ASTC, and changed the terminal
   fallback to `R8G8B8A8_UNORM`. The silent `KTX_TTF_NOSELECTION` branch now
   logs. `tests/src/texture_vkt_tests.c` sweeps all 1024 combinations of texture
   class, device type, sRGB intent, and the five capability flags, asserting
   every selector result has a transcode target.
10. **Add device-memory pooling and budget telemetry.** Device/type/heap
   telemetry **done 2026-07-31** and explicit logical-owner attribution **done
   2026-08-01**; pooling deliberately not started. All device allocations now
   route through `vulkan_backend_allocate_device_memory` /
   `vulkan_backend_free_device_memory`, which record into a handle-keyed table
   so live counts remain exact while that table has capacity; saturation marks
   them inexact. `VK_EXT_memory_budget` is enabled when present for driver heap
   usage. `vkr_renderer_get_device_memory_stats`
   reports live/peak/total allocation counts and bytes globally and for eleven
   fixed owner buckets, plus the per-memory-type distribution, logged by the app
   at startup, scene load-ready, and unload. The metrics registry keeps
   live/peak as gauges and publishes lifetime-total differences as interval
   counters. Owners are caller-declared and stored in the live handle table;
   they are never inferred from names or memory types.
   `find_memory_index_with_fallback` replaces three hand-rolled retry
   strategies, one of which (`vulkan_image.c`) had no fallback at all.
   **Baseline captured** — see
   [performance/gpu-memory-baseline.md](../performance/gpu-memory-baseline.md).
   On Apple M1 Pro / MoltenVK loading Sponza: peak 206 live allocations against
   a `maxMemoryAllocationCount` of ~1.07e9, 2281 MB resident, and a strongly
   bimodal distribution (DEVICE_LOCAL mean 14.5 MB across 156 allocations;
   host-visible mean 0.47 MB across 50). **The measurement argues against
   writing a pooling allocator now:** the count is nowhere near any limit, the
   large allocations are already block-sized, and the only population pooling
   would help totals ~1% of device memory. Revisit on a device reporting a low
   allocation limit.

10b. ~~**The backend-selection ladder does not survive a third backend.**~~
    **Resolved 2026-08-08; cross-platform evidence completed 2026-08-09.** The
    former 46-site, 14-file ladder is replaced by one immutable
    `VkrRendererImplCapabilities` record and a coarse selected strategy. Metal
    and legacy Vulkan were the two real V2 implementations; the bindless identity
    then gained a production offscreen V3/V4 slice, and a normal frame makes exactly the
    prepare and submit indirect calls. `VkrRendererImplSubmitResult` replaces the untyped Metal
    pointer and carries the shared capture, memory, material, and pass-timing
    data. A renderer-source audit finds backend-type behavior only in factory
    selection. ADR-026 later removed the legacy implementation, adaptor, and its
    migration-only invariant assertion.
    [ADR-025](adr/025-selected-renderer-implementation-strategy.md) is Accepted.
    The Windows CPU/runtime witnesses and the matched clean Release Metal
    profile pass their declared gates.

15. **The editor viewport topology is only partially connected.**
    `assets/render_graphs/main.rendergraph.json` declares the complete editor
    branch — `scene_color`, `scene_depth`, and `scene_pre_transmission` at
    `extent:{mode:viewport}`, plus `Editor.Composite` and `UI.Editor` — gated on
    the `editor_enabled` condition. Metal now assigns
    `packet.frame.editor_enabled` immediately before graph build, so the branch
    and P18 editor transmission chain are reachable there. Vulkan
    still patches only picking, transmission, and shadow conditions, so its
    editor condition remains false and `UI.Fullscreen` always builds.

    `prepared_frame.viewport_width/height` has the same defect from the other
    direction: both backends initialize it to the window size in `prepare_frame`
    and never update it from `packet.frame.viewport_*`, so
    `extent:{mode:viewport}` is indistinguishable from `extent:{mode:window}`.
    The packet's viewport values reach shader constants only.

    Fixing both is phase P0 of
    [ui-architecture-spec.md](../ui/ui-architecture-spec.md) and a prerequisite
    for any editor UI work. It enables three previously-unbuilt passes, so it
    requires a Debug validation-layer run on both backends rather than a CPU
    suite alone.

### P2 — Throughput

11. ~~**Cull before materializing world payloads.**~~ **Shipped 2026-07-31, and
    measured as a no-op on current content.** Visibility is classified per
    submesh from world-space spheres derived with the largest model-matrix
    column length, so non-uniform scale over-estimates rather than clipping.
    On Sponza it tests 36 submeshes and rejects **0**: the glTF importer merges
    primitives by material, so each submesh spans the whole model (radii 6.5–9.6
    against a model radius of 10.2) and no spatial locality remains to cull
    against. On **San Miguel** the same code rejects **102–107 of 282**
    submeshes (~37%) as the camera moves, which is what culling looks like on
    content that kept its spatial locality. See
    [performance/p2-throughput-findings.md](../performance/p2-throughput-findings.md).
12. ~~**Use real instancing first.**~~ **Shipped 2026-07-31.** Opaque draws are
    sorted by merge key and runs sharing geometry, generation-bearing material,
    index range, pipeline domain, and position-independent binding context
    collapse into one instanced draw. Instance records are emitted in run order
    so each run is contiguous. Local reflection-probe descriptors are selected
    once per draw from world position; while a local probe is pending or ready,
    a unique binding-context key prevents objects at different positions from
    being merged and receiving the first object's probe state. Pending probes
    are included because the IBL pass can make one ready after packet building.
    On measured content it merges **zero** draws. The merge sort also supplies
    the counters, avoiding the original duplicate key allocation and duplicate
    `qsort`.
13. ~~**Use MDI only for meaningful binding-state groups.**~~ **Shipped
    2026-07-31.** Draws accumulate into a bounded batch and are submitted with
    `vkCmdDrawIndexedIndirect` when more than one shares all bound state,
    falling back to direct draws otherwise (also when `multiDrawIndirect` or
    `drawIndirectFirstInstance` is unavailable). The measurement makes the
    governing rule explicit: **MDI's reach is set by how much state a pass binds
    per draw, not by how many draws it has.** The world pass batches nothing —
    every draw changes descriptor set, since materials own one each. The
    depth-only shadow pass binds its pipeline and instance state once for the
    whole list, so all 281 opaque casters × 4 cascades are carried by **8
    indirect calls**. The P2 review fixed indirect submission to bind the same
    optional compacted opaque index buffer as its direct fallback; previously it
    always rebound the geometry's default index buffer, so capability could
    change which indices were rendered. Metrics now distinguish logical
    commands from actual direct/indirect calls. Frame time was unchanged in the
    original A/B (20.57 ms with, 20.91 ms without), because the render graph's
    entire CPU cost is 0.3–0.5 ms of a 20 ms frame.
14. ~~**Keep camera and shadow visibility separate.**~~ **Shipped 2026-07-31.**
    The shadow payload previously aliased the world payload's arrays, so the
    moment culling rejected anything every camera-culled object would silently
    stop casting a shadow. Camera and light visibility are now classified
    independently in one traversal. Shadow visibility is the union of all
    cascade volumes: cascade centers shift with their camera slices, so the last
    or widest cascade is not guaranteed to contain earlier ones. The original
    P2 implementation tested only the last cascade and could drop a near-only
    caster; a focused regression now pins the union rule.

### P3 — Feature growth after measurement

- automatic exposure, bloom, and post-processing beyond manual exposure plus
  the initial ACES tonemap;
- real compute dispatch followed by GPU culling/compaction;
- clustered/tiled light assignment and a storage-buffer light list;
- shader hot reload with pipeline/descriptor invalidation rules.

The former GPU-address renderer proposal is no longer P3 work. Metal Stages 0–5
and Vulkan V0–V7 ship as the two selected strategies; the owner accepted
the corrected Bistro/text result, the shared-core witnesses pass, and ADR-026
removed the legacy Vulkan migration surface. The 2026-08-12 post-V7 RX 6700 XT
rerun closes the target correctness boundary. Corrected local bindless baseline
proposals remain intentionally removed, and no performance conclusion follows
from completion.

---

## 9. Portfolio Assessment

The renderer demonstrates broad, relevant engineering skill:

- a non-trivial C11 engine and renderer split;
- explicit Vulkan resource/synchronization work rather than wrapper-only use;
- a data-authored graph with scheduling, culling, resources, and timing;
- allocator/lifetime discipline and reload-aware ownership policies;
- shader reflection, persistent pipeline cache, compressed textures, async CPU
  loading, ECS/editor integration, CSM, text, and GPU picking;
- a meaningful CPU test suite.

The most important portfolio weakness is not missing visual effects; it is that
some flagship abstractions are only partially authoritative. A graphics
reviewer will notice the per-draw world loop, blocking out-of-frame upload and
readback-wrap paths, and undeclared IBL bake work inside a graph executor.
Closing or measuring those boundaries—and exercising failure paths and a wider
device/swapchain matrix—will strengthen the project more than adding another
isolated effect.

A fair current description is: **strong engine/rendering architecture and
breadth, with remaining gaps in whole-frame graph authority, cross-device
failure validation, GPU allocation policy, and draw throughput.**

---

## 10. Verification Record

Reviewed statically on 2026-07-31 against the current working tree. The
following checks were rerun:

| Check | Result |
|---|---|
| `./build_test.sh` | Exit 0; all registered test suites completed |
| `./build_test.sh` after P0 review | Exit 0; all registered suites completed, including 10 render-graph barrier/error-contract tests and the framebuffer slice-view regression test |
| `./build.sh Debug` after P0 review | Exit 0; application and shaders built successfully |
| Validation-layer run after P0 | Apple M1 Pro/MoltenVK, three-image swapchain: startup, IBL setup, swapchain recreation, and 10 seconds of steady frames completed with no validation messages after fixing the layer-0 array-view mismatch |
| `./build_test.sh` after P1 review | Exit 0; picking lifecycle regressions, strict uniform-layout negatives, all 13 shipped shader manifests, and all 1,024 texture-selector combinations passed |
| `./build.sh Debug` after P1 review | Exit 0; shaders, texture packer, renderer library, and application built successfully |
| Retired `./validate_pipeline_cache.sh` after P1 review | Historical exit 0; cold cache created/saved and warm cache loaded; 20 pipelines created in both runs. Current cache validation uses two production application processes with one fresh explicit cache path |
| P1 Sponza validation-layer run | Apple M1 Pro/MoltenVK, three-image swapchain: Sponza ready in 33.1 Debug seconds, 50-second run clean, exact device-memory telemetry, and zero fence/queue/device upload waits |
| `tools/validate_multithreaded_backend_matrix.sh` after P1 review | Exit 0; all five compile/runtime configurations passed |
| `./build_test.sh` after P2 review | Exit 0; coordinate-convention, cascade-union, merge-context, orthographic-frustum, and single-target graph regressions passed with every registered suite |
| `./build.sh Debug` after P2 review | Exit 0; application and shaders built successfully |
| P2 Sponza validation-layer run | Apple M1 Pro/MoltenVK, three-image swapchain: exact Sponza scene activated in a 50-second Debug run, no validation messages observed, and upload-wait counters remained zero |
| P2 front-face correction validation | Paired viewpoints reproduced reversed one-sided surfaces; after switching immutable pipeline front-face state to CCW, a direct Sponza capture showed the intended interior wall with skybox and both reflection probes intact, no validation messages, and zero upload waits |
| `tools/validate_multithreaded_backend_matrix.sh` after P2 review | Exit 0; all five compile/runtime configurations passed |
| `./build_release.sh` and P2 Release Sponza smoke | Exit 0; 192 samples in 50 seconds, 16.957 ms mean frame and 0.156 ms mean render-graph CPU. This is runtime evidence, not a comparison with the different-scene San Miguel baseline |
| Metrics phase 1 | CPU suite and Debug build passed; validation-layer snapshot exposed 155 slots and eight provenance-carrying pass rows with no drops; Release Sponza activated with all asset-event classes; compile-disabled Release passed; pipeline-cache cold/warm validation consumed 23 events per run. Five paired Release A/B blocks measured a +0.243299% median frame delta with a -0.877233% to +0.462384% range. Full configuration and repetitions: [phase-1 verification](../tooling/renderer-metrics-phase1-verification.md) |
| Metrics phase 1b | CPU tracker/catalog/delta tests and full suite passed; Debug validation exposed 66 owner rows as 44 live/peak gauges and 22 allocated/created counters with no running-total rows or drops; earlier Sponza snapshots proved exact owner/global live sums and zero live `unknown`/`staging`; compile-disabled Release and pipeline-cache cold/warm workflows passed. Five paired Release A/B blocks measured a -0.064025% median frame delta with a -0.363189% to +1.188834% range. Full configuration and repetitions: [phase-1b verification](../tooling/renderer-metrics-phase1b-verification.md) |
| Renderer harness phase 2 | CPU suite and Release build passed; a hidden-window Sponza profile completed two isolated warm-cache repetitions with stable warmup, matching work-volume rows, 222 aggregate metrics, eight pass rows, bounded event aggregation, verified child/artifact digests, and a passing work-volume assertion. Compile-disabled behavior and exact commands are recorded in [phase-2 verification](../tooling/renderer-harness-phase2-verification.md) |
| Renderer harness phase 2b | The final CPU suite and Release build passed; a 600-frame deterministic Sponza observation exposed 222 metric/eight pass rows with matching work volume, and a timestamp-on run produced complete CPU/GPU samples for all eight passes. Authoritative profiles now require multiple isolated repetitions, clean provenance, stable warmup, an exclusive GPU lane, and complete requested evidence; the legacy app log/shell scrape is retired. The clean baseline's unrelated event-test intermittent and exact artifacts are recorded in [phase-2b verification](../tooling/renderer-harness-phase2b-verification.md) |
| Renderer harness phase 3 | CPU suite and Release build passed; five full and five automation Sponza repetitions reported masks `0x000000000007ffff` and `0x000000000000bfff`, identical draw/visibility work, complete `boot.*` and exact GPU residency, a 5.64% lower observational `boot.systems` mean, and 1,494,056 fewer resident GPU bytes. A validation-layer automation profile completed with no validation diagnostics. These dirty local observations are not an authoritative speed claim; exact artifacts and limitations are recorded in [phase-3 verification](../tooling/renderer-harness-phase3-verification.md) |
| Renderer harness phase 4 | CPU suite and Debug build passed; a fixed asynchronous capture ring and request-specific exact-slice graph overlay produced canonical final color, swapchain/editor depth, shadow-layer, scene-color, and picking-ID artifacts. Two isolated non-editor Sponza snapshot processes had bit-identical data/preview digests; an editor replay resolved depth to `scene_depth`; Debug validation logs were clean and the five-case backend matrix passed. These diagnostic dirty-tree runs are not performance or baseline evidence; exact artifacts and limitations are recorded in [phase-4 verification](../tooling/renderer-harness-phase4-verification.md) |
| Renderer harness phase 5 | CPU suite and Debug build passed; six logical Sponza debug channels replayed independently with distinct canonical digests and clean validation logs. Canonical color/depth/ID comparison, diff reporting, aggregate summary transport, primary-plus-snapshot autotest separation, no-mutation proposals, digest confirmation, immutable generations, and atomic current-pointer publication are implemented. The real baseline tree was not mutated; successful acceptance was verified under an isolated temporary repository root. Exact artifacts and limitations are recorded in [phase-5 verification](../tooling/renderer-harness-phase5-verification.md) |
| Renderer harness phase 6 | CPU suite and Debug build passed; the five-case backend matrix passed. Surface-free two- and three-image offscreen targets completed explicit recreation and validation-clean Sponza runs. A three-image windowed counterpart exercised swapchain recreation; all 80 deterministic work metrics matched across targets, and canonical final-color/depth digests were byte-identical. Exact artifacts and local MoltenVK limitations are recorded in [phase-6 verification](../tooling/renderer-harness-phase6-verification.md) |
| HDR environment/IBL deterministic and build gates | `./build_test.sh`, `./build.sh Debug`, `./build_release.sh`, the now-retired pipeline-cache wrapper, and all five multithreaded-backend matrix configurations passed at the time; focused tests cover RGBA16F metadata/lowering, binary16 boundaries, HDR orientation/aspect/cache bypass/cleanup, scene source alternatives, cube derivation/direction mapping, and GGX source LOD |
| HDR validation snapshot | Debug Apple M1 Pro/MoltenVK, offscreen 640×480 with two images and validation enabled: full HDR upload/conversion/convolution/BRDF/skybox/world/tonemap/capture/shutdown path passed with no VUID/error/fatal diagnostics; `scene_color` captured `R16G16B16A16_SFLOAT`; both local probes reached ready. Report `20260803T205636.755Z-01405b`, digest `sha256:09b1ab3b0cd8349eb1f2f05ea982f61cb7215cf053351eee063e6de21b2b191e` |
| HDR cubemap seam correction | Debug Apple M1 Pro/MoltenVK, offscreen 960×720 with two images and validation enabled: the camera points exactly through the `+X/+Y/+Z` cube corner, and the final Citrus Orchard capture is continuous across all three faces with no VUID/error/fatal diagnostics. CPU tests cover all 12 shared edges and all 8 corners. Report `20260803T214124.246Z-0172b1`, digest `sha256:a5f0c7a3fff4d14d2287da68a9a4515dc02587afaa67fde22d0ab10a532b1afb` |
| HDR Release boot observation | Five Release repetitions at 2560×1440, hidden immediate-present window, three images, isolated warm cache: all passed with exact GPU totals and zero upload fence/queue/device-idle waits. Non-authoritative because the local profile permits dirty provenance and warmup was unstable; GPU timing was disabled, so this is not a speed result. Report `20260803T205253.704Z-013cad`, digest `sha256:00db04dd1bec14efb4df09674315de961fb82e8c6ce88ebaff0c7c50fa39d455` |
| HDR manual-exposure equivalence | Debug Apple M1 Pro/MoltenVK local offscreen replay captured `final_color` plus canonical RGBA16F `scene_color` at exposure `0.30`; their 1,228,800 byte values differ by mean `0.0163` and at most one quantization step. No VUID/error/fatal diagnostics. Report `20260804T090320.491Z-00c100`, digest `sha256:4de538b3c698428afe37b9c5f5f9dae431523765b0c4e5b76ed0e3eba3f176e2` |
| Bistro HDR baseline safety | Final exposure `0.30`/sun `0.75` run completed all five fixed views with clean validation logs; full-resolution inspection found continuous sky and bright-luma coverage of `0.05%-1.05%`, down from `6.44%-21.38%` before calibration. Report `20260804T085151.570Z-00b294`, digest `sha256:8ebe00c3a2c596482049b9dbdb0be523cbd5790b60d2df65675e233c57bab678`. The pre-seam proposal is obsolete; no baseline was proposed or accepted |
| Bistro alpha-mask shadow routing and PCF origin | `./build_test.sh` and `./build.sh Debug` passed. Five-view validation snapshot `20260804T105412.251Z-010923` completed all replay children with 20 alpha-tested calls per cascade and clean logs; aggregate exit 1 is only the intentional pre-HDR baseline mismatch. Direct `shadow_cascade_0` snapshot from the final rebuilt binary, `20260804T110741.708Z-0115f8`, exited 0 and shows fine branch/foliage silhouettes. Both are non-authoritative local/dirty correctness observations; no baseline was proposed or accepted |
| Bistro shading completion focused snapshots | Final Debug reports passed for spec-gloss unlit parity (`20260804T150302.250Z-0053fa`), layered transmission fullscreen/editor (`20260804T150225.308Z-005383`, `20260804T150236.277Z-0053c5`), broad-mesh local IBL (`20260804T150248.830Z-0051d5`), and five-position shadow factor (`20260804T145726.557Z-004c7f`). The motion aggregate and all children share one workload fingerprint backed by a published 689-asset scene-content manifest; completed child logs contain no validation diagnostics. These are local/dirty correctness observations, not performance evidence |
| Bistro shading correction | Owner review invalidated snapshot `20260804T151505.151Z-00644f` and its unaccepted proposal: the real Bistro scene had no environment controls or local probe, stale mesh-cache material references bypassed the corrected converter, and the converter omitted the Khronos sub-F0 dielectric branch. The invalid plan `20260804T152024.898Z-0064a8` must not be accepted. After generated namespace v2/mesh cache v10, authored café IBL, shared probe resources, and a real analytic-light diagnostic mode, the hidden-window report `20260804T163924.287Z-0099c7` matches the corrected interior path. Paired interior final/HDR/unlit/analytic/shadow captures pass in `20260804T170935.156Z-00b50b`; paired exterior final/analytic/shadow captures pass in `20260804T172035.207Z-00bcd0`, where same-façade lit/shadow receiver samples align with factor `175/255` versus `0/255`. The definitive five-view run `20260804T170504.198Z-00b23e` has five passing children, matching fingerprint `sha256:f69476b246d98fc2edddffeae76963822fbe22b5c41eb8f6228b2b66455e6f22`, empty validation stderr, and a 1,442-asset manifest including present prepared-cache sidecars; aggregate exit 4 is expected baseline incompatibility. Corrected generation `sha256:364c0da293733ec9ec6e59996f2707a3df6573f0347a3087ae70aabc3cb43b95` is proposed by plan `20260804T171130.209Z-00b65a`, digest `sha256:c7d22dff733c8aa68ddd4ecf8858a67dadb53ee078b985bc763d4c5771005d00`, and remains unaccepted |
| Bistro camera-light/Fresnel correction | Owner cameras `[-35.9016342, 5.81466007, -7.73436356]` and `[-29.3909912, 6.29170275, -9.78384113]` exposed camera-ranked light membership and an implicit white F90 on zero-specular legacy materials; therefore the second unaccepted proposal recorded in the preceding row is also invalid and must not be accepted. The fixed six-channel replay `20260804T200153.974Z-011f62` passes, digest `sha256:f2248e5a4cd3d5dccec164edfe11e7957291c78d84bb38737d86b290a6a88eb3`; both final-color endpoints remove the circular sign highlight, every child records a constant 72-light scene table with zero table drops over all seven frames, and stderr contains no validation diagnostics. The final five-view replay `20260804T202607.453Z-013306`, digest `sha256:138efcb9fab10b0079eb4f0115f6b138cf16a641d986b0c4d3879ebe53ee8112`, has five passing children, one workload fingerprint, all 72 scene lights, and clean validation logs. Its no-mutation proposal is plan `20260804T203308.761Z-01348d`, digest `sha256:70ac562cfa980fef21ddeaf4567b6b3f0e9ab17af3fba5027518d4d97045f044`, generation `sha256:2b7d1f695fbc6a715b63a4be980a0d5331a939feecb4a9ae5ca29085c6252abf`; it remains unaccepted and the accepted pointer is unchanged. CPU tests, pipeline-cache validation, and the five-case backend matrix pass. A matched local/dirty 3200×2400 Release observation `20260804T200948.429Z-0120be`, digest `sha256:b1f1eb28c3d00458e0568583884a506d78f061139841f6acb4e148aacc3ed9db`, has stable warmup and unchanged 206-draw work; CPU submit p50 is 4.21 ms versus 5.32 ms before, while opaque GPU p50 is 93.27 ms versus 87.32 ms. The fingerprints differ, so the timings are diagnostic and not an authoritative performance claim |
| Bistro fragment-light/specular-IBL correction | Five further owner cameras invalidated the receiver-level 12-light completion claim: their diagnostic replay retained all 72 scene lights but measured 44,200 influencing receiver/light pairs and discarded 34,305, leaving valid fixtures dark. Final/unlit/analytic/shadow plus no-specular and no-IBL ablations initially appeared to isolate a separate moving wall boundary to specular IBL. ADR-019 now owns a camera-independent 384-cell world grid with full 128-bit masks; fragments apply exact range/cone rejection, while the PBR path adds normal-footprint roughness filtering, specular AO, and horizon rejection. The optimized five-camera replay `20260804T223449.580Z-000d77`, digest `sha256:2de757f5aa6d221fdf03b53d80f98ea2c0a691c695e28d5f6b50c1d55d5136e1`, has five passing children and 35 passing metric assertions: all 72 lights remain with zero drops, using 363 cells, 1,635 references, and at most 47 candidates per cell. Same-fingerprint local/dirty optimization reports `20260804T221657.247Z-01866a` → `20260804T222805.692Z-000857` reduce World Opaque p50 from 109.528 ms to 93.243 ms; the older receiver report's 93.272 ms has a different workload fingerprint and is context only. That historical snapshot was not validation-clean because the then-open 17-vs-16 sampler-limit VUID occurred at startup; §8 records its subsequent resolution. The prior proposal remains invalid and no replacement has been accepted |
| Bistro face-orientation/indoor-probe correction | The final owner audit superseded the wall's specular-IBL attribution: direct-diffuse and normal captures showed a sharp view-vector `faceforward` flip on two-sided geometry, while shadow factor remained stable. PBR now uses `SV_IsFrontFace`, samples diffuse irradiance from the surface normal without box projection, and uses box projection only for specular reflection rays. Bistro's café volume now references an authored indoor diffuse cubemap with local specular intensity zero rather than reusing the outdoor environment. PBR reflects 17 sampled images and 13 sampler descriptors through explicit semantically identical aliases. Each IBL conversion/convolution owns immutable descriptor state released against the submit serial, preventing later probe bakes from invalidating recorded commands. The exact five-camera 1600×1200 replay `20260805T102236.609Z-01020d`, digest `sha256:ee1810dbaa479cf627458f24de79096a97ec12850f07b55658b1a46a2728d6de`, has five passing children with workload fingerprint `sha256:b99fd4de02f3e8c76938d83cc4ab17d4c4d3bc2bede84b5a6e76cbdff970ad97`; full-resolution review shows no sharp camera-translating wall boundary or outdoor environment wash in the café, and logs contain no VUID/error/fatal/sampler-limit/invalid-command diagnostics. This is local/dirty correctness evidence, not an authoritative baseline or performance result; no replacement baseline has been proposed or accepted |
| Bistro curved-wall shadow audit | Three later 1600×1200 owner cameras isolate a separate curved-wall boundary to raw directional shadow factor and direct diffuse. Exact diagnostic `20260805T120354.711Z-0177c1`, digest `sha256:5d77c58149cf0e14b7aef5db3c4bcd85430398c05545f03b389156fc99f2afe0`, excludes unlit, normals, material parameters, direct specular, and IBL. Cascade replay `20260805T122914.160Z-006da8` places the edge inside cascade 1 with identical 234 opaque indirect commands, 20 alpha calls, and zero shadow-union culls at all positions. Scene-depth replay `20260805T130951.874Z-013a91`, digest `sha256:b50cdffd044c9aeaa48330050264f7718ce47def78e1a9776a1a342ddda22c98`, proves screen-row-600 edge pixels x=934 and x=393 unproject to the same wall point within 5.6 cm. The edge is a world-stationary sun shadow revealed by camera parallax, not a camera-locked renderer artifact; experimental scene-bounds wiring did not change it and was discarded. Runs are local/dirty correctness evidence with clean diagnostics, not an accepted baseline or performance result |
| Bistro golden baselines | The tracked legacy Vulkan generation `sha256:c3596ff14cdf206d0be4138840957925bd18353dd5d8eab339bfdec575df3564`, committed before V7, remains the historical cross-backend visual reference; it is not post-retirement implementation evidence. Metal generation `sha256:3db4f4d2294e5fdbc3618e64c4b2baf03bf66051dee0c4ff452e341d20cae51d` remains immutable but historical. Corrected bindless Bistro/text output was visually accepted on 2026-08-11, but its local proposals/generations were removed at the owner's request and are not retained authorities. |
| Metal visual-parity correction | Corrected fourteen-view Metal snapshot `20260807T194321.543Z-0040dc` was compared directly with the accepted Vulkan generation: normalized per-view final-color MAE is `0.00662–0.03607` (mean `0.02060`), and the formerly divergent exterior-sky view drops from `0.1569` to `0.01153`. A matched channel audit found near-exact unlit (`0.00007`) and material-parameter (`0.00104`) output, isolating the remaining difference to normals/lighting rather than stretched texture coordinates. The production text fixture is byte-identical across backends and three isolated Metal runs are deterministic. Transmission routing emits both fixture draws. Hidden-window report `20260807T202738.082Z-01208a` completed two passing 34-frame Metal children with empty stderr; its aggregate is unavailable only because the generic profile requires immediate presentation while Metal reports FIFO and the short warmup was unstable. These are local/dirty correctness observations; no baseline was mutated or performance claim made. |
| Bindless Metal root-ABI closure | Apple M1 Pro / Metal 4 builds, host regressions, and native nested pipeline reflection pass after correcting minimum-alignment validation and Slang `uint3` padding. Release offscreen text report `20260812T163826.676Z-01310f` captures exact final color `sha256:019ba7752b653ea77dc8fce8e4125b042f67794d1978aa12044ed4c5b44ad3a6` and picking IDs `sha256:ed47dbf1b5e6ade6820370e0313b257c3067d667dd277a1d0033c4d204a26388`; focused Debug report `20260812T163901.620Z-0136ab` reproduces both bytes with Metal API and GPU/shader validation enabled and no diagnostic beyond the enablement notices. These local/dirty runs close ABI and correctness scope only; they are not a baseline or performance result. |
| Vulkan V1–V4 migration | Complete on the RX 6700 XT. V4 uses keyed DEVICE/UPLOAD/READBACK buffer/image pools, maintenance4 pre-creation requirements, dedicated-hint handling, persistent host mapping, device-local staged geometry, and complete logical/physical metric projection. Native window/reacquisition synchronization, GPU-assisted validation, exact readback, submit-value staging retirement, balanced retirement/collection, and logical totals returning to baseline are implemented. macOS builds the shared code and passes the CPU contract but cannot execute the descriptor-buffer backend. ADR-024's cross-platform extraction witnesses are complete. |
| Vulkan V5–V7 | V5/V6 native Windows evidence covers the authored synchronization2/dynamic-rendering graph, analytical IBL, shared capture, timestamps, cache, lifecycle, metrics, and validation. V7 removed the legacy renderer and migration surface. Post-V7 CPU and Metal gates pass. On 2026-08-12, Debug and Release offscreen text-graph reports each passed two repetitions with three images and nine pass rows; focused offscreen/windowed synchronization and GPU-assisted validation also passed on the RX 6700 XT. These are correctness witnesses, not performance evidence. |
| `vkr_frustum` production references | Application world-payload construction creates camera and cascade frustums and classifies submeshes against them |
| `VkrDrawBatcher` production references | Removed. It never gained a production caller after GPU compaction replaced it, and ADR-028 P21 deleted `vkr_draw_batch.*`; the symbol appears in no build output. |
| Generic indirect-draw subsystem | Removed by V7; selected implementations own packet draw submission |
| `vkAllocateMemory` call sites | Vulkan has pooled-block allocation plus required dedicated buffer/image paths, all accounted by its keyed memory adapter |
| VMA | Not present |
| Dynamic rendering | Implemented for the complete Vulkan 1.4 authored graph; compiled synchronization2 barriers, graphics scopes, transfer copies, and compute IBL execute through the selected Vulkan strategy |
| Descriptor indexing/bindless | Descriptor-buffer sampled/storage/sampler arrays are implemented and validation-clean in the Vulkan 1.4 path; there is no descriptor-set fallback |
| Bindless packet ABI reflection | Shared host ABI records are covered by the CPU suite. Recursive SPIR-V layout and descriptor checks run when native Vulkan pipelines are created; the fresh post-V7 focused Windows diagnostic passes this gate. |
| Image-state lowering | Canonical graph barriers are covered by the CPU suite and lowered privately to synchronization2 by the Vulkan implementation. Upload, IBL, capture, and presentation transitions remain explicit Vulkan-private barriers; the legacy layout-pair helper and `vulkan_image.c` were removed by V7. |

On 2026-08-21 part of that matrix was closed on the RX 6700 XT. Debug
validation-layer runs of the deferred-only topology pass on two-image
(`local.p21.vulkan.offscreen_2image`, `20260821T111543.460Z-002735`) and four-image
(`local.p21.vulkan.offscreen_4image`, `20260821T111555.540Z-003f61`) WSI-free targets, with the report
confirming actual image counts of 2 and 4, and on requested-pixel picking
(`local.p21.vulkan.picking_hit`, `20260821T111607.646Z-0035e3`), all with no VUID, validation error,
device-loss, renderer-error, or fatal marker.

The rest of the matrix is still not covered, and three parts of it are blocked
by absent capability rather than by unspent effort:

- **Other GPUs and drivers.** One device is available here.
- **Windowed two-/four-image swapchains.** The harness rejects
  `target_image_count` on windowed cases by design — the WSI selects it — so
  only the WSI-free target can vary image count.
- **Queue-family layouts.** The Vulkan device selects a single
  graphics+compute+transfer(+present) family; there is no separate-transfer-queue
  configuration to vary. Varying it is backend work, not a test run.
- **Minimize/cancel.** The Windows resize round trip now passes, but zero-extent
  suspension and cancellation have no focused harness witness.
- **Injected acquire, fence, submit, and present failures.** No fault-injection
  facility exists in the renderer or the harness.

The Windows resize diagnostic now also exercises the destruction boundary that
previously produced `VUID-vkDestroySemaphore-semaphore-05149` and
`VUID-vkDestroySwapchainKHR-swapchain-01282`. The platform resize mailbox is
consumed before frame preparation, and fallback retired-target collection waits
for successor acquire-submit completion when present fences are unavailable.
`smoke.sponza.windowed_resize` final report `20260821T124314.444Z-000821`,
digest
`sha256:280927c35ca78de4f035f6673a5631310bb84a49e23559251df74b8e09487ad0`,
passes two Debug repetitions on the RX 6700 XT and observes
`320x240 -> 400x300 -> 320x240` without a VUID, validation error, device loss,
renderer error, or fatal marker. The local profile, dirty tree, and unstable
three-frame warmup make it a non-authoritative correctness diagnostic.
