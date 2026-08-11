---
status: implemented
updated: 2026-08-11
authority: spec
---
# VKR Renderer — Architecture and Status Specification

**Document status:** Reviewed against the working tree on 2026-07-31; P2
production-reference record corrected on 2026-08-01, harness Phases 3-6 status
recorded on 2026-08-02, nested glTF texture/specular-glossiness diffuse
compatibility recorded on 2026-08-03, and HDR environment/IBL plus tonemap
activation recorded on 2026-08-03 with seam-safe cube baking corrected on
2026-08-04; Bistro alpha-mask shadow routing and the PCF hash-grid origin were
corrected on 2026-08-04; prepared specular-glossiness lowering, graph-declared
transmission feedback, bounded glTF punctual lights, caster-relevant cascade
fit, fragment-local IBL, and transitive scene-content fingerprints were added
on 2026-08-04; the reviewed fourteen-view Bistro golden baseline was accepted
on 2026-08-05; the optional Metal 4 Stages 0–5 path was recorded on 2026-08-06,
and cross-backend text parity plus backend-pinned Vulkan/Metal Bistro-plus-text
evidence were recorded on 2026-08-07; production shader sources were moved
behind their backend owners on 2026-08-07; the Metal generation was reclassified
as historical after the parity correction on 2026-08-08; the V1 shared-core
characterization and V2 selected implementation seam were recorded on
2026-08-08; matched Release performance and explicit macOS native-resize
validation pass; native Windows CPU and resize-validation evidence completed
V1/V2 on 2026-08-09; the selected production bindless Vulkan V3/V4 slices,
shared GPU cores, native RX 6700 XT window/reacquisition path, and V4 asset
publication were completed and validated on Windows on 2026-08-09; the
post-extraction macOS CPU, byte-identical Metal snapshot, Metal API/GPU
validation, and paired Release witnesses were recorded on 2026-08-09. The
V4 keyed Vulkan memory pools, complete memory metric projection, and post-change
RX 6700 XT validation gate were completed on 2026-08-10. The Windows V5
authored graph, dynamic-rendering pass set, PBR/IBL path, shared asynchronous
capture, timestamps, and offscreen/windowed validation gates were completed on
2026-08-10; V6 selection, cache/lifecycle/metrics, and matrix support also ship.
A later Bistro visual audit fixed premature global-HDR bake cancellation,
generation-blocking deferred logical unpublication, normalized local-cubemap
rejection, and incomplete presentation-lifetime proof.
The owner visually accepted the corrected Bistro/text output on 2026-08-11.
Local snapshots, reports, proposals, bindless baseline generations, and the
legacy-only performance experiment were removed afterward at the owner's
request. Packet-native editor/gizmo initialization completes the full-boot
contract. Windows V6 and Gate B1 are complete, bindless Vulkan is the Windows
default, and explicit `vulkan` retains Vulkan 1.2 as a diagnostic fallback. The
macOS extraction witnesses remain open outside the completed Windows V6 slice.
**Scope:** Renderer architecture, implemented features, CPU/GPU memory, data
transfer, synchronization, known issues, and recommended direction.
**Audience:** Contributors and reviewers.
**Companion:** [Architecture Decision Records](adr/README.md)

This is a status snapshot, not a promise that every design document under
`docs/` describes current code. Paths and symbols are preferred over volatile
line counts.

---

## 1. Executive Summary

VKR is a substantial Vulkan 1.2 renderer and engine framework written in C11.
Its strongest implemented work is architectural: a public frontend/backend
boundary, packet-based frame submission, a JSON-authored render graph, SPIR-V
reflection for descriptor and vertex layouts, explicit allocator families,
asynchronous CPU resource preparation, an ECS scene model, editor rendering,
picking, CSM, text, glTF, a KTX2/UASTC asset pipeline, and a half-float HDR
environment/IBL path with tonemapped presentation.

It is beyond tutorial scale, but several claims in the earlier version of this
document overstated how completely those systems are integrated:

- The render packet is a useful frame contract, but submission begins the
  Vulkan frame before packet validation and mutates retained renderer systems.
  It is packet-based, not a purely stateless or replayable renderer.
- The render graph schedules and instruments declared work, and barrier
  execution now derives Vulkan access and stage masks from the compiler's
  access flags with per-(mip, layer) state. Picking is now fully declared;
  runtime IBL baking still performs nested rendering outside graph-owned
  passes, so graph inference is authoritative only for declared resources.
- SPIR-V reflection drives descriptor layouts, push constants, vertex ABI, and
  now uniform members, whose offsets and sizes are cross-validated against
  `.shadercfg` at shader creation.
- Resource preparation is asynchronous, and frame-path upload finalization no
  longer blocks: a full Sponza load measures zero render-thread fence waits,
  queue waits, and device waits. Uploads outside an active frame still block,
  and parallel upload command pools still require an explicitly unsafe opt-in.
- The world path culls, merges compatible instanced draws, and submits
  multi-draw-indirect where binding state permits. On the measured content the
  payoff is confined to the shadow pass, which carries 1,124 depth-only commands
  in 8 indirect calls; the world pass batches nothing because each material owns
  a descriptor set. Frame time is unchanged either way — the whole render graph
  is ~2% of a 20 ms frame. See
  [performance/p2-throughput-findings.md](../performance/p2-throughput-findings.md).

The correctness issues that previously took precedence over new visual features
— frame streams indexed by swapchain image modulo three, backend frame failures
returned as success, render graph execution failures that could not reach packet
submission, and rejected packets presenting with an assumed old layout — were
resolved on 2026-07-31 (§8, P0). Their hardware verification still depends on a
broader validation matrix; a three-image Apple M1 Pro/MoltenVK Debug smoke run
is clean, while resize/minimize, other image counts, and injected failures
remain to be exercised.

The project remains a strong renderer showcase. Its next step should be to make
the current architecture trustworthy under failure and across swapchain
configurations, then demonstrate measured throughput improvements.

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
    ├── vkr_render_graph.*     Graph declaration and resource management
    ├── vkr_rg_compile.c       Dependencies, scheduling, culling, barriers
    ├── vkr_rg_execute.c       Pass execution and timing
    ├── vkr_rg_json.c/h        JSON graph parsing and realization
    ├── passes/                Named pass executors
    ├── systems/               Scene-facing renderer subsystems
    ├── resources/loaders/     Asset loaders
    ├── metal/                 Metal packet backend and production shaders
    └── vulkan/                Legacy and bindless Vulkan implementations,
                               production shaders, reflection
app/src/main.c                 Sample/editor application
assets/                        Shader manifests/SPIR-V, materials, scenes, data
tools/                         Offline utilities and focused validation stages
tests/src/                     CPU-side unit and subsystem tests
docs/                          Specifications, plans, investigations, ADRs
```

The renderer frontend owns the renderer subsystems and selects one coarse
`VkrRendererImpl` strategy. The retained legacy Vulkan strategy reaches its
backend through `VkrRendererBackendInterface`; Metal and bindless Vulkan own
their packet paths directly. Public resources use opaque handles, and Vulkan
types stay behind the backend boundary. The legacy adaptor still exposes
concepts shaped by Vulkan's render-pass and descriptor model.

---

## 3. Frame Architecture

### 3.1 Actual frame sequence

The normal frame path is:

1. `vkr_renderer_prepare_frame()` calls `vkr_renderer_begin_frame()`.
2. The backend waits for a frame-slot fence, acquires a swapchain image, resets
   and begins that image's command buffer.
3. Async resource finalization is pumped and the instance/indirect stream
   cursors are reset.
4. The application creates `VkrRenderPacket` and its arrays in caller-owned
   scratch memory.
5. `vkr_renderer_submit_packet()` validates the packet, applies retained text/UI
   updates and globals, realizes the JSON graph for the current frame, executes
   it, ends the command buffer, submits, and presents.

This ordering matters: swapchain acquisition and command recording have
already begun by the time validation runs. An invalid packet is therefore not
a zero-side-effect rejection.

### 3.2 Packet-based submission

`VkrRenderPacket` version 3 contains frame information, globals (including
manual HDR exposure), and optional
world, shadow, skybox, UI, editor, picking, text-update, and debug payloads.
Important properties are implemented:

- optional payload pointers control pass participation;
- version and field validation produce `VkrValidationError` with a field path;
- payload arrays remain caller-owned and must live until submission returns;
- draw items refer to stable renderer handles rather than Vulkan objects;
- passes retrieve typed payloads from `VkrRgPassContext`.

The boundary should be described as **packet-based** or **value-like**, not
fully stateless:

- `prepare_frame` is a required, ordered operation;
- submission updates retained globals, UI/text resources, descriptor state,
  graph caches, upload queues, and metrics;
- the packet is not self-contained enough to serialize and replay without the
  retained handle registries and subsystem state;
- validation failures call `vkr_renderer_end_frame()` after image acquisition.

### 3.3 JSON render graph

`assets/render_graphs/main.rendergraph.json` is parsed into a reusable JSON
model at renderer initialization. On every submitted frame,
`vkr_rg_build_from_json()` resets and realizes the active resources and passes
for current dimensions, editor condition, and cascade count; `vkr_rg_execute()`
compiles the graph if needed and runs it.

Implemented graph capabilities:

- declared image/buffer reads, writes, and attachments;
- dependency construction, topological ordering, and pass culling;
- conditions and `${i}` repeat expansion;
- runtime extent/format aliases;
- named executor registry;
- graph-owned/imported resources and generation-bearing handles;
- CPU timing and optional buffered GPU timestamp results;
- live/peak graph-resource statistics.

The source JSON contains eight resource declarations and twelve pass
declarations. Conditions gate five resources: `scene_color`/`scene_depth` on
`editor_enabled`, `hdr_scene_color` on `!editor_enabled`, and
`picking_color`/`picking_depth` on `picking_pending`. The fullscreen/editor
variants are alternatives, not additive passes; fullscreen world/sky feed the
tonemap pass while editor world/sky feed viewport composition. The four-cascade
shadow repeat expands at build time.

#### Synchronization boundary

Barrier execution is access- and subresource-aware for declared resources:

- barriers carry `src_access`/`dst_access` and a subresource range through to
  `vkr_renderer_image_barrier`, which derives Vulkan access and stage masks from
  them rather than from an old/new layout pair;
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

The graph is also not the sole authority over current GPU work. Picking now
uses graph-owned attachments and a separately declared transfer read for its
pixel copy. The IBL executor still records several prepared graphics passes
whose internal resources and accesses are not declared in the JSON graph. This
must be fixed before graph inference can be treated as a correctness guarantee
for the whole frame.

`VKR_RG_RESOURCE_FLAG_TRANSIENT` currently means graph-owned/reusable rather
than “freed after each frame”: resources survive between realizations and are
recreated when their resolved description changes. Transient aliasing is not
implemented.

### 3.4 Shader and pipeline construction

SPIR-V reflection is implemented with `spirv_reflect` and currently supplies:

- descriptor sets, bindings, descriptor type/count, and block byte sizes;
- push constant ranges;
- vertex inputs used to build/validate supported host vertex ABIs;
- semantic role resolution for frame/draw sets and bindings;
- uniform block members, including scoped names, offsets, sizes, scalar/vector/
  matrix shape, array count/stride, and matrix stride.

`.shadercfg` remains important for shader identity, stages, render pass,
`vertex_abi`, and the frontend's named uniform declarations. Shader creation
cross-validates those declarations against the reflected frame/draw block:
scope, name, offset, exact byte size, scalar/vector/matrix shape, array count
and stride, and matrix stride must agree. Matching blocks contributed by
multiple stages must also have identical member layouts. Slang-generated
matrix/array wrapper structs are normalized before comparison. Reflection does
not yet flatten arbitrary user-authored nested structs into the manifest's
flat declaration model, and reverse completeness is intentionally not required:
a shader may contain a member the host never writes.

Static assertions correctly pin `VkrVertex3d`, `VkrInstanceDataGPU`, and
`VkrIndirectDrawCommand` host layouts. Pipeline cache persistence is also
implemented.

Production source ownership follows the backend boundary. Both
`lib/src/renderer/vulkan/shaders/` and `lib/src/renderer/metal/shaders/` are
organized by render domain (`common`, `world`, `shadow`, `picking`, `text`,
`skybox`, `ibl`, and `post`), with Vulkan-only `ui` and `viewport` domains kept
explicit. CMake maps the reorganized Vulkan modules to the stable
`assets/shaders/*.spv` names consumed by `.shadercfg`; those generated modules
remain runtime assets rather than source authority. Metal's domain Slang and
native MSL inputs are compiled/aggregated into one generated runtime library.
No shader source or backend-specific renderer validator is owned by `tools/`.
The CPU suite owns deterministic Metal memory/lifetime, packet-ABI,
capture-ring, material-table, and dependency-lowering checks. Backend-pinned
harness snapshots exercise the production Metal shader artifacts, graph/pass
execution, capture path, pixels, and API/GPU validation.

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
the alpha-tested shadow list. The PCF hash grid converts the fitted light-view
origin into the shader's reconstructed right/up basis, including the negated X
axis introduced by `mat4_look_at`. Two-sided PBR shading uses
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
the Khronos sub-F0 non-metal rule; companion image version 1 and mesh-cache
version 12 invalidate incomplete or stale prepared references. Generated image
and material-file publication is atomic and resumable.

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

---

## 4. Feature Status

| Area | Status | Current boundary |
|---|---|---|
| Selected renderer implementation | Implemented, partial migration | One coarse `VkrRendererImpl` strategy selects Metal, retained legacy Vulkan, or bindless Vulkan once; the legacy 86-operation adaptor remains until ADR-026 retirement |
| Packet submission | Implemented, partial | Versioned/validated with a real cancel path, but ordered and state-mutating |
| JSON render graph | Implemented, partial | Scheduling/culling/timing; access- and subresource-aware barriers for declared resources; IBL bake work remains undeclared |
| SPIR-V reflection | Implemented | Descriptors, push constants, vertex ABI, and uniform members validated against `.shadercfg` |
| Pipeline cache | Implemented | Disk-backed Vulkan cache |
| Metrics registry and snapshot export | Implemented | Bounded typed slots, MPSC cold-event ring, triple-buffered snapshots, renderer catalog/validity, explicit GPU allocation-owner aggregates, metrics-backed HUD, atomic `--metrics-json`, and harness aggregation |
| Renderer automation harness | Implemented | Strict cases/profiles, deterministic cameras, isolated repetitions, dependency-resolved boot, authoritative evidence policies, metric/pass/event aggregation, atomic artifacts, direct and auxiliary captures, canonical comparison/diffs, separated `autotest`, guarded immutable baselines, target-neutral windowed or true surface-free offscreen execution with actual configuration provenance, and a sorted transitive scene-content manifest—including prepared glTF material files, generated textures, and present packed siblings—whose digest participates in workload identity |
| Cascaded shadow maps | Implemented | Four-cascade default with debug/fit controls; cutout casters use the alpha-tested path, PCF grid coordinates share the shader basis, and opt-in scene-bounds Z fit clips caster bounds against each final cascade XY rectangle |
| PBR materials | Implemented, evolving | Metallic-roughness and texture slots plus prepared, cached specular-glossiness lowering with retained dielectric F0/F90 response; transmission adds IOR, volume, attenuation, and scene-color refraction while clearcoat and sheen remain absent |
| IBL | Implemented, partial integration | HDR/cubemap sources, prepared RGBA16F bakes, global environment, and two fragment-weighted local probes per draw ship; bake work remains undeclared to the graph and explicitly barriered |
| glTF and scene loading | Implemented | CPU async pipeline; nested texture URIs and sidecars resolve without flattening; UVs lower once to VKR convention; point, spot, and directional punctual lights import through the scene transform into a stable 128-light table with a fragment-local 384-cell bitmask grid; frame-path uploads measured non-blocking |
| Transmission | Implemented, initial | Graph-declared opaque, HDR feedback-copy, transmission, and ordinary-blend stages; screen-space refraction has no order-independent transparency or deep compositing |
| KTX2/UASTC textures | Implemented | BC7/BC5, ASTC, ETC2, EAC RG11, and RGBA32 paths; every selector result is transcodable |
| Editor viewport and picking | Implemented | Picking fully declared in the render graph; readback usually deferred but ring wrap can block |
| Text | Implemented | Bitmap, MTSDF, system-font, UI and world text paths; the dedicated harness fixture produces byte-identical final RGBA output on legacy Vulkan and Metal, with deterministic backend-specific picking coverage |
| Instance stream | Implemented, underused | Fixed 65,536 entries; measured content repeats no compatible asset/state run |
| CPU frustum culling | Implemented | Per-submesh; camera and union-of-cascade light visibility classified independently; rejects ~37% on San Miguel, 0% on Sponza |
| Draw batching | Implemented | Opaque draws merge by complete compatible state; local-probe descriptors prevent unsafe world instancing across positions |
| Multi-draw indirect | Implemented | Fires where a pass binds state once: 1,124 shadow commands → 8 indirect calls; world pass batches nothing until descriptor state can be shared |
| Compute dispatch | Not exercised | “compute” JSON passes currently orchestrate graphics/CPU work |
| Device-memory suballocation | Implemented in bindless Vulkan | The Vulkan 1.4 path uses keyed DEVICE, UPLOAD, and READBACK pools backed by `vkr_gpu_memory`. Buffers and images are segregated, address-bearing buffer blocks alone carry the device-address allocation flag, maintenance4 requirements and dedicated hints are queried before object creation, and host-visible blocks are persistently mapped. Logical and physical totals, peaks, owner classes, retirement, free space, failures, and capacity lower into renderer metrics. Legacy Vulkan remains per-resource. |
| Bindless/descriptor indexing | Implemented, partial Metal and Vulkan paths | Metal 4 Stages 0–5 implement GPU-addressed buffers, native texture/sampler material rows, placement/ring retirement, backend-lowered graph dependencies, all authored pass categories, PBR/lighting/IBL/transmission/text/picking, capture, metrics, and pipeline archives. The application selects it with `--renderer metal`, shared loaders publish generation-safe assets plus writable environment bake targets, and backend-pinned harness cases prevent accidental renderer substitution. Packet version 10 separates the visible skybox from the lighting IBL source. Metal graph ownership is isolated from async resource allocation, frame-only schedule scratch is scoped, bounded command-slot waits publish as `frame.command_slot_waits`, and failed texture unpublication preserves logical ownership for retry. The formerly accepted Metal Bistro generation is historical after a parity audit exposed retained IBL, sampler, transparency, and presentation defects; corrected Gate A pixel acceptance remains open. The selected Vulkan 1.4 bindless strategy completes RX 6700 XT V3–V6: immutable ADR-023 device selection, descriptor buffers, queue/timeline/command ring, completed acquire-wait-submit WSI with optional present fences, production SPIR-V ABI reflection, keyed memory pools, shared asset publication, authored synchronization2/dynamic-rendering graph execution, full PBR/IBL/text/picking passes, asynchronous capture, and completion-valid metrics/timestamps. Gate B1 is complete and bindless Vulkan is the Windows default. Explicit `vulkan` selects the retained Vulkan 1.2 diagnostic path; `vulkan-bindless` remains available for pinned migration cases. |
| Bindless Vulkan V1–V4 migration | Complete for RX 6700 XT | V1 characterization and V2's selected strategy are complete on macOS and Windows. V3 extracted the memory, submit-ring, and shared ABI cores alongside their production Vulkan callers and passes native window resize with reacquisition and retired-swapchain completion proof. V4 extracted the slot table, added completion-gated asset publication, and moved geometry, staging, images, startup buffers, and readback into keyed dynamic pools with complete logical/physical metrics. Prepared and writable initialization records before the next frame draw, staging retirement uses that submit value, publication dirty ranges flush once per backing buffer, and logical totals return to baseline. MoltenVK cannot execute the descriptor-buffer path. ADR-024 remains partial only for post-capture-extraction Metal regression witnesses, not bindless Vulkan implementation. |
| Bindless Vulkan V5/V6 migration | Windows V6 complete; bindless is the Windows default | V5 lowers the authored graph to synchronization2/dynamic rendering and implements shadow, skybox, world alpha domains, picking, tonemap, editor, UI/text, compute IBL, five-channel asynchronous capture, and completion-valid pass timestamps. Analytical nine-mip IBL and whole-graph offscreen/windowed validation pass on the RX 6700 XT. Logical unpublication frees the active ID immediately while full-handle pending work retains the retired physical generation through submission; local probes accept sampled normalized/sRGB sources. Packet-native editor/gizmo initialization completes full boot, and packet upload publishes the required zero-overflow metric. V6 selector, present mode, pipeline-cache, asset lifecycle, metrics parity, and Windows implementation-matrix support are implemented. The owner visually accepted corrected Bistro/text output. Local evidence artifacts were removed on request. Gate B1 is complete, bindless is the Windows default, and legacy Vulkan remains explicitly selectable. The required macOS extraction witnesses remain open outside Windows V6. |
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

Vulkan `VkAllocationCallbacks` use a dedicated `VkrDMemory` reservation plus a
small refcounted command-scope arena. This accounts driver host allocations in
the project's allocator statistics. It is unrelated to GPU device-memory
suballocation.

---

## 6. GPU Memory

There is no VMA. The two Vulkan implementations deliberately use different
device-memory policies during migration:

- Bindless Vulkan 1.4 suballocates buffers and optimal-tiling images from keyed
  DEVICE, UPLOAD, and READBACK blocks through `vkr_gpu_memory`. Pool keys include
  resource kind, exact memory type, and device-address requirement; host-visible
  blocks are persistently mapped. Required and initially preferred dedicated
  allocations bypass the range core while retaining the same logical and
  physical metrics.
- Legacy Vulkan 1.2 still calls `vkAllocateMemory` per image, buffer
  create/resize, and readback buffer through its tracked allocation/free pair.
  A `VulkanBuffer` may additionally use `VkrDMemory` for ranges within one
  deliberately shared buffer; that does not pool arbitrary Vulkan resources.

The bindless adapter publishes separate physical-allocation and logical-
suballocation totals, peaks, buffer/texture owner classes, retirement, free
space, largest range, bounded failure classes, and capacity. The legacy tracker
publishes live/peak/total native allocation counts and bytes globally and by
logical owner, per-memory-type distribution, heap capacities, and driver heap
usage/budget when `VK_EXT_memory_budget` is available. If its fixed handle table
saturates, it marks live totals and owner live/peak rows inexact. The graph also
tracks sizes of its own resources. Neither implementation currently provides
defragmentation/eviction or transient image/buffer aliasing.

The legacy per-resource policy has been measured rather than inferred: see
[performance/gpu-memory-baseline.md](../performance/gpu-memory-baseline.md).
The earlier claim that Sponza/San Miguel approach a particular allocation limit
is **refuted** on the measured configuration — Sponza peaks at 206 live
allocations against a limit of ~1.07e9.

---

## 7. CPU↔GPU Communication and Synchronization

### 7.1 Per-frame mapped data

`VkrInstanceBufferPool` owns three persistently mapped buffers with a fixed
65,536-instance capacity. Overflow is reported and the affected pass can drop
work; it is not silent. `VkrIndirectDrawSystem` mirrors the design for 16,384
commands and is consumed by world/shadow MDI groups, with direct fallback on
unsupported features or stream exhaustion.

Global shader UBOs use swapchain-image-indexed regions, and global descriptor
sets are allocated/indexed per swapchain image. Descriptor updates are cached
using generations plus concrete image view/sampler/buffer payloads. Instance
descriptor-state retirement is delayed by submit serial.

Two capacity values must not be conflated: the backend object descriptor pool
may grow up to 8,192 states, while shader-system CPU tracking currently starts
with/fixes a separate 1,024-instance capacity depending on the path.

### 7.2 Resource preparation and upload

`VkrResourceLoader` separates worker-thread `prepare_async` from render-thread
`finalize_async`. The resource pump has count/op/byte budgets and guarantees
forward progress by permitting the first oversized request.

Upload helpers no longer block the render thread during a frame: when a frame is
being recorded they record into the active command buffer and defer both the
submission and the staging teardown, and refuse rather than wait if they cannot.
Measured over a full Sponza load, this yields zero render-thread fence, queue,
and device waits (§8, P1 item 8). Uploads that run outside an active frame still
wait on their fences.

A dedicated transfer queue is selected when the device exposes a separate
family, including queue-family ownership work. Staging is retired
asynchronously: the deferred-destroy queue is gated on submit serials against
`completed_submit_serial`, which only advances once a frame-slot fence proves
GPU completion. Out-of-frame upload helpers still submit and wait.

Per-worker transfer/graphics-upload command pools are created only when both
`VKR_PARALLEL_UPLOAD` and `VKR_PARALLEL_UPLOAD_UNSAFE=1` enable the experimental
parallel runtime. It is not the default shipped upload path.

### 7.3 Readback

Picking uses a three-slot host-visible readback ring. Result polling normally
checks completion without waiting and may return a result one or more frames
later. If a new request wraps onto a still-pending slot,
`renderer_vulkan_request_pixel_readback()` waits indefinitely on the associated
frame fence. The correct description is **deferred in the common case**, not
“never blocking.”

### 7.4 Frame synchronization

The WSI object sizing is deliberate:

| Object | Count/index |
|---|---|
| Acquire semaphores | max frames in flight / current frame slot |
| Submit fences | max frames in flight / current frame slot |
| Render-complete semaphores | swapchain image count / acquired image |
| Image-in-flight fence references | swapchain image count / acquired image |

Per-image render-complete semaphores avoid re-signaling a semaphore still owned
by presentation. `max_in_flight_frames` is capped by `BUFFERING_FRAMES` (3).
All per-image arrays use the image count returned by
`vkGetSwapchainImagesKHR`, which may be larger than the requested minimum.

The frame implementation's P0 defects were resolved on 2026-07-31 (§8):

- instance and indirect streams are indexed by the fence-protected
  frame-in-flight slot, and the slot advances exactly once per frame;
- fence wait, acquire, command-buffer begin/end, queue submit, and present
  failures return distinct errors, with `VKR_RENDERER_ERROR_FRAME_SKIPPED`
  separating a recreate-and-skip from device loss;
- `vkr_rg_execute()` returns `VkrRendererError`, so compile, barrier, and
  begin/end-pass failures reach `vkr_renderer_submit_packet()`;
- a rejected packet goes through `vkr_renderer_cancel_frame()`, which
  resets the partially recorded primary command buffer, rolls back frame-local
  readback/timing state, and records only a discard-to-present transition;
- failures after acquire that cannot submit mark the frame for WSI/sync-object
  recovery before the next acquire.

`vkDeviceWaitIdle` is not part of the normal successful submit loop, but it is
used by resize/shutdown and some graph timing/cache/resource recreation paths.
Upload paths instead contain blocking fence waits.

---

## 8. Prioritized Issues

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
    and legacy Vulkan are real implementations, the bindless identity now owns
    a production offscreen V3/V4 slice, and a normal frame makes exactly the
    prepare and submit indirect calls. `VkrRendererImplSubmitResult` replaces the untyped Metal
    pointer and carries the shared capture, memory, material, and pass-timing
    data. A renderer-source audit finds backend-type behavior only in factory
    selection; the legacy Vulkan backend retains one invariant assertion.
    [ADR-025](adr/025-selected-renderer-implementation-strategy.md) is Accepted.
    The Windows CPU/runtime witnesses and the matched clean Release Metal
    profile pass their declared gates.

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
- shader hot reload with pipeline/descriptor invalidation rules;
- the proposed native-Metal/modern-Vulkan
  [GPU-address renderer](bindless-gpu-pointer-renderer-spec.md), whose focused
  Metal Stages 0–5 now ship as an optional application/harness path with
  production asset publication, memory retirement, explicit-ID material rows,
  graph lowering, authored packet-pass execution, pipeline archives, capture,
  retained text, lighting/IBL, and metrics. The historical guarded generation
  remains immutable, but corrected output intentionally differs and Gate A
  awaits owner-reviewed replacement pixels; the default-backend change remains
  pending. The modern-Vulkan Windows target now executes its complete authored
  graph through the V5 bindless path, including dynamic rendering, IBL,
  asynchronous capture, and timestamps. The owner visually accepted corrected
  Bistro/text output; local evidence artifacts were removed afterward on
  request. Windows V6 and Gate B1 are complete, so bindless is the Windows
  default. Required macOS post-extraction witnesses remain open as V3–V5
  cross-platform evidence, not as Windows V6 work.

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
| `./validate_pipeline_cache.sh` after P1 review | Exit 0; cold cache created/saved and warm cache loaded; 20 pipelines created in both runs |
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
| HDR environment/IBL deterministic and build gates | `./build_test.sh`, `./build.sh Debug`, `./build_release.sh`, `./validate_pipeline_cache.sh`, and all five multithreaded-backend matrix configurations passed; focused tests cover RGBA16F metadata/lowering, binary16 boundaries, HDR orientation/aspect/cache bypass/cleanup, scene source alternatives, cube derivation/direction mapping, and GGX source LOD |
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
| Bistro golden baselines | The legacy Vulkan generation `sha256:c3596ff14cdf206d0be4138840957925bd18353dd5d8eab339bfdec575df3564` remains the cross-backend visual reference. Metal generation `sha256:3db4f4d2294e5fdbc3618e64c4b2baf03bf66051dee0c4ff452e341d20cae51d` remains immutable but is historical. Corrected bindless Bistro/text output was visually accepted on 2026-08-11, but its local baseline generations were removed at the owner's request and are not retained authorities. |
| Metal visual-parity correction | Corrected fourteen-view Metal snapshot `20260807T194321.543Z-0040dc` was compared directly with the accepted Vulkan generation: normalized per-view final-color MAE is `0.00662–0.03607` (mean `0.02060`), and the formerly divergent exterior-sky view drops from `0.1569` to `0.01153`. A matched channel audit found near-exact unlit (`0.00007`) and material-parameter (`0.00104`) output, isolating the remaining difference to normals/lighting rather than stretched texture coordinates. The production text fixture is byte-identical across backends and three isolated Metal runs are deterministic. Transmission routing emits both fixture draws. Hidden-window report `20260807T202738.082Z-01208a` completed two passing 34-frame Metal children with empty stderr; its aggregate is unavailable only because the generic profile requires immediate presentation while Metal reports FIFO and the short warmup was unstable. These are local/dirty correctness observations; no baseline was mutated or performance claim made. |
| Bindless Vulkan V1–V4 migration | Complete on the RX 6700 XT. V4 uses keyed DEVICE/UPLOAD/READBACK buffer/image pools, maintenance4 pre-creation requirements, dedicated-hint handling, persistent host mapping, device-local staged geometry, and complete logical/physical metric projection. Native window/reacquisition synchronization, GPU-assisted validation, exact readback, submit-value staging retirement, balanced retirement/collection, and logical totals returning to baseline are implemented. macOS builds the shared code and passes the CPU contract but cannot execute the descriptor-buffer backend. ADR-024 remains partial only for unavailable post-capture-extraction Metal witnesses. |
| Bindless Vulkan V5/V6 migration | The Windows implementation executes every authored pass category through synchronization2 and dynamic rendering, including analytical nine-mip IBL, shared asynchronous five-channel capture, and completion-valid CPU/GPU pass rows. Whole-graph offscreen/windowed and GPU-assisted validation complete without actionable diagnostics. Pipeline-cache, lifecycle, metrics, and implementation-matrix support are present. Packet-native editor/gizmo initialization supplies the full-boot contract, and packet submission supplies complete instance-overflow metrics. The owner visually accepted corrected Bistro/text output; local snapshots, profiles, proposals, and bindless baseline generations were removed on request. Gate B1 is complete, bindless Vulkan is the Windows default, and explicit `vulkan` retains the legacy diagnostic path. Required macOS extraction witnesses remain open outside Windows V6. |
| `vkr_frustum` production references | Application world-payload construction creates camera and cascade frustums and classifies submeshes against them |
| `VkrDrawBatcher` production references | None outside its module/tests/docs; P2 batching instead uses visibility and pass-local batch structures |
| `vkr_indirect_draw_alloc` callers | The shared pass indirect-submission path allocates production world/shadow command ranges |
| `vkAllocateMemory` call sites | Legacy allocations remain centralized in its tracked wrapper. Bindless Vulkan has one pooled-block allocation site plus dedicated buffer/image paths, all accounted by its keyed memory adapter |
| VMA | Not present |
| Dynamic rendering | Implemented for the complete Vulkan 1.4 authored graph; compiled synchronization2 barriers, graphics scopes, transfer copies, and compute IBL execute through the selected bindless strategy |
| Descriptor indexing/bindless | Descriptor-buffer sampled/storage/sampler arrays are implemented and validation-clean in the Vulkan 1.4 V4 path; legacy Vulkan 1.2 remains descriptor-set based |
| Uniform block reflection output | Populated; validated against all 16 `.shadercfg` files by `reflection_pipeline_test` |
| `vkr_renderer_transition_texture_layout` callers | Replaced by `vkr_renderer_image_barrier`; the layout-pair table survives only for upload/copy paths in `vulkan_image.c` |

The CPU suite and one MoltenVK smoke configuration do not replace
validation-layer runs across two- and four-image swapchains, queue-family
layouts, other GPUs, resize/minimize/cancel paths, interactive picking, and
injected acquire, fence, submit, and present failures.
