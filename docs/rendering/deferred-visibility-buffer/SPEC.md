---
status: implemented
updated: 2026-08-23
authority: spec
---

# GPU-Driven Deferred Visibility-Buffer Renderer

The P0-P3 graph, lifetime, format, shared-ABI, and megabuffer foundations are
implemented on both backends. Metal P4, P6, P8, P10, P12, P14, P16, P17, P18,
and the P19 transmission-compaction path now ship in the accepted
P20 default topology: GPU frustum/HZB culling and four-bucket ICB
submission, opaque and transmission visibility buffers, compute material
resolve and deferred lighting, four-layer depth-peeled transmission shading, a
completion-gated HZB history ring, requested-pixel visibility picking, and
camera-plus-cascade multi-view indirect submission. Windows Vulkan
P5/P7/P9/P11/P13/P15 and P16-P18 execute the same authored topology, and the
accepted P20 stabilization makes it the default there, with the native P21
Vulkan gate passing on 2026-08-21 after repairing a retirement defect that
turned the bounded geometry/material publication boundary into a hard frame
failure (§11.1):
GPU classification/compaction, indirect-count visibility and cascade raster,
G-buffer resolve and deferred lighting, completed-history HZB, requested-pixel
picking, and four transmission peel/shade layers. Target RX 6700 XT Debug
validation and Release execution pass. Vulkan samples normal/ORM/emissive maps,
spells the shared light/refraction model, and now publishes completion-gated
per-layer transmission coverage. The dominant three-fixture normal-X mismatch
is corrected. The owner accepted the classified remainder of 520 double-sided
secondary-side pixels, 147 shared raster-edge pixels, and 2,410 pixels in the
separate transmission debug composite for the P20 visual threshold. P20 was
owner-accepted on both backends on 2026-08-20 without promoting a snapshot
baseline. P21 is implemented: the renderer now exposes one world topology,
with no runtime selector or legacy whole-frame fallback.
P19 now provides the default Metal `Transmission.Compact` path. Each layer uses
one 8×8 scan to copy its background, compact final visibility winners into a
viewport-sized bounded pixel list, accumulate count/overflow, and invoke the
one-thread indirect-argument finalizer in the same compute encoder after an
explicit dispatch-to-dispatch device-visible barrier. A sparse indirect shade
dispatch consumes the list. One list and one argument buffer are reused
serially across layers in each completion slot. This consolidation reduces the
compact chain from twelve graph passes to eight. Completion-gated coverage and
overflow metrics remain available, and `VKR_TRANSMISSION_COMPACT_DISABLED=1`
selects the retained full-screen Metal diagnostic path. World and editor
captures are byte-identical to full-screen controls; the complete CPU suite,
serial API-plus-shader validation, and isolated cold/warm cache runs pass. A
matched local dirty-tree Release observation reports 3.204 ms of
non-diagnostic transmission GPU work versus 6.095 ms for full-screen and a
timestamp-off frame mean of 20.279 ms versus a contemporaneous 20.943 ms
full-screen control. The evidence remains local, dirty, and warmup-unstable, so
it supports the owner-selected Metal default but is not a portable speed claim.
Unsupported packet structure and exhausted candidate capacity are rejected
before backend recording; neither backend silently changes topology. Missing
Metal deferred pipelines fail backend initialization. `VKR_HZB_DISABLED=1`
remains the focused P14 diagnostic rollback to frustum-only GPU culling.
Rationale and the durable decisions this specification constrains are in
[ADR-028](../../architecture/adr/028-gpu-driven-deferred-visibility-buffer.md).

## Executive summary

- Replace forward opaque shading with an 8-byte visibility buffer, a 12-byte
  compute-materialized G-buffer, and compute lighting.
- Keep ordinary alpha blend, world text, UI text, and UI as feature-local
  direct-raster compositing passes inside the deferred renderer; they are not a
  selectable legacy renderer topology.
- Treat each GPU culling input as an instance × submesh draw candidate; compact
  candidates into visible rows, state-bucketed indirect arguments, and counts.
- Add graph-driven compute, realized graph buffers, named binding resolution,
  indirect-read synchronization, and completion-gated lifetimes before adding
  rendering features.
- Write deferred lighting to `hdr_pre_transmission`, copy it to
  `hdr_scene_color` through ADR-018's declared feedback pass, then composite
  transmission and ordinary blend into scene color.
- Use a dedicated HZB history ring. The baseline is conservative previous-frame
  occlusion with invalidation; current-frame two-phase refinement is optional.
- Start transmission with a full-screen sparse compute dispatch. Pixel-list
  compaction is added only if measurement justifies its producer and buffers.
- Preserve picking resolve/readback and the feature-local blend/text picking
  raster; remove only redundant opaque/transmission re-rasterization.
- Implement Metal first for each feature slice, then mirror it on Vulkan before
  advancing to the next layer of the renderer.
- The P20 migration retained the legacy renderer until visual parity, GPU
  correctness, work-volume equivalence, and matched Release evidence were
  accepted.
- P21 deletes the legacy topology, whole-frame/material/overflow reroutes, and
  the `deferred_enabled` migration flag. Runtime rollback has ended;
  source-control rollback remains available.

## 1. Intent and scope

### 1.1 Problem

Before this migration, the opaque world path rasterized and shaded in one
fragment invocation. Each surviving or overdrawn fragment could evaluate base color, alpha test,
normal mapping, ORM, emissive, a directional light and shadow sample, the
ADR-019 punctual-light grid, global and local IBL, and screen-space
transmission. Submission was also CPU-driven per draw, with visibility decided
in `vkr_visibility.c` and backend draw state/root data encoded for every
survivor.

The selected architecture separates three costs:

1. Rasterization records the winning draw and primitive.
2. Material resolve evaluates the winning surface once per covered pixel.
3. Lighting consumes a coherent screen-space G-buffer once per covered pixel.

Alpha-cutout base color is necessarily sampled once during raster acceptance
and again during full material resolve. “Once per visible pixel” refers to the
full material evaluation, not that cutout acceptance sample.

This separation is a hypothesis, not a performance result. It is accepted only
if matched Release evidence shows that reduced overdraw and CPU submission cost
pay for the added full-screen storage and compute traffic.

### 1.2 In scope

- Opaque and alpha-cutout visibility, material resolve, and deferred lighting.
- Transmissive visibility and fused deferred transmission shading.
- Instance × submesh candidate generation, GPU frustum culling, compaction, and
  indirect execution.
- Multi-view candidate generation and GPU submission for shadow cascades, so
  that CPU per-submesh visibility and draw merging have no surviving world
  consumer at retirement.
- A vertex/index megabuffer and the GPU tables required to address it.
- Conservative HZB occlusion after the frustum-only path is accepted.
- Mesh picking that resolves object IDs from visibility data without rerasterizing
  opaque or transmissive meshes.
- Bounded layered transmission by depth peeling, required only if the
  single-layer fidelity decision is rejected.

### 1.3 Out of scope

- Ordinary alpha-blended shading, world text, UI text, and UI; these remain
  direct-raster compositing feature passes in the sole deferred topology,
  not an alternative whole-frame forward renderer.
- Mesh shaders, meshlets, ray tracing, temporal antialiasing, and MSAA.
- Order-independent transparency for *ordinary alpha blend*. Blend is correct
  today via back-to-front submission and is not a retirement blocker. Multi-layer
  *transmission* is separately in scope; §11 explains why depth peeling, not an
  OIT accumulation scheme, is the tool for it.
- Replacing ADR-019's light-assignment model.
- Unrelated format gaps such as `D24_UNORM_S8_UINT`, and MRT pipeline support;
  the G-buffer is written by compute and the visibility pass has one
  color attachment.

### 1.4 Reference boundary

The design follows published visibility-buffer and deferred-texturing
techniques, including analytic barycentric gradients for explicit texture LOD.
The owner's reference point was the RE Engine family. No implementation detail
of a shipped Capcom title is asserted because it is not verifiable from this
workspace.

## 2. Starting implementation fit and blockers

### 2.1 Existing pieces that fit

- Vertex data is already pulled through GPU addresses in the packet shaders.
- Materials are flat, immutable, indexable GPU rows.
- The graph already carries a working `R32_UINT` picking image and declared
  readback path.
- The BRDF, punctual-light-grid, probe, and shadow helpers are largely
  derivative-free and can move to compute.
- Conditional graph passes/resources, dynamic rendering, synchronization2,
  timeline semaphores, and per-subresource image state already exist.
- Metal already addresses index data by GPU address.

### 2.2 Foundation contract and implementation status

| # | Missing contract | Required change |
|---|---|---|
| B1 | Graph buffers are parsed and barrier-planned but not realized by either packet backend | Realize declared buffers with explicit lifetime, capacity, usage, and completion-gated reuse |
| B2 | Graph compute passes do not carry a generic executable dispatch | Add pipeline/kernel identity plus direct and indirect dispatch descriptors; remove per-frame string dispatch from the hot path |
| B3 | Compute-write → indirect-read synchronization is not expressible | Add buffer access `INDIRECT_READ`, stage `DRAW_INDIRECT`, and backend lowering including Vulkan `VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT`; apply it to argument and count buffers |
| B4 | Image reads are resolved by ordinal and parsed `binding` values are ignored | Resolve every image/buffer use by declared binding and validate missing, duplicate, or type-mismatched bindings at realization |
| B5 | Required storage/attachment formats and usage combinations are incomplete | Add `R32G32_UINT` and `R16G16_SNORM` mappings and query exact sampled/storage/color-attachment/transfer support for visibility, G-buffer, HZB, HDR, and depth-seed resources before enabling the path |
| B6 | Graph images cannot author mip counts or per-use mip slices | Add `mip_levels` and image-use subresource ranges; create per-mip views/descriptors and declare every HZB dependency |
| B7 | Neither backend executes a GPU-generated command stream today | Implement and capability-gate a Metal 4 indirect/ICB strategy and Vulkan indirect-count execution |
| B8 | Geometry is published as separate vertex/index buffers | Add stable-generation vertex/index megabuffers, publication tables, retirement, and Vulkan index-buffer device-address usage |
| B9 | Draw state is not represented by GPU compaction | Pin triangle-list/u32-index input and compact into legal cutout/cull-mode state buckets |
| B10 | Schema/parser condition parity was incomplete | Keep HZB-history validity and transmission/picking conditions aligned across authored schema and parser validation; P21 removed the temporary topology condition |

As of 2026-08-17, B1-B10 are implemented on both backend code paths, including
Vulkan indirect-count execution and four legal cutout/cull-mode partitions.
The macOS evidence gate exercised focused
Metal API/shader validation, GPU candidate compaction, ICB execution,
visibility-buffer rasterization, material resolve, and megabuffer growth.
The native Windows gate exercises the corresponding Vulkan graph on an RX 6700
XT with validation enabled. Wider driver/device coverage remains open because
this repository intentionally marks Vulkan initialization unsupported on macOS.

The graph schema already supports buffer resource declarations. The gap is
realization and the missing access, dispatch, lifetime, binding, and subresource
semantics—not generic buffer syntax.

## 3. Target frame topology

The renderer has one world topology. P21 removed the temporary
`deferred_enabled` condition and the legacy graph branch. The surviving world
topology is:

```text
Shadow.Cascade.${i}       graphics  unchanged
IBL.Bake                  compute   unchanged
Cull.Draws                compute   candidates + optional valid HZB history
                                     -> visible rows + bucket args/counts
VBuffer.Opaque            graphics  indirect buckets -> opaque vbuffer + depth
GBuffer.Resolve           compute   opaque vbuffer -> GB0/GB1/GB2;
                                     initialize hdr_pre_transmission
Lighting.Deferred         compute   read/write hdr_pre_transmission;
                                     add lighting or write sky
HZB.BuildMip.${i}         compute   current depth -> current HZB history slot
World.FeedbackCopy        transfer  hdr_pre_transmission -> hdr_scene_color
Transmission.DepthSeed.N transfer  opaque depth -> transmission depth layer N
VBuffer.Transmission.N   graphics  indirect buckets + prior peel depth
                                     -> transmission vbuffer/depth layer N
Transmission.Shade.N     compute   back-to-front source + layer N visibility
                                     -> ping-pong HDR composite
World.Blend               graphics  unchanged, composites hdr_scene_color
Post.Tonemap              graphics  unchanged
UI                        graphics  unchanged
```

The editor topology uses the identical ordering with
`scene_pre_transmission -> scene_color`. No executor-owned hidden copy or
transition is permitted: all reads, writes, and the feedback copy remain graph
uses.

`GBuffer.Resolve` initializes every `hdr_pre_transmission` pixel: covered
pixels receive emissive, and empty pixels receive zero. `Lighting.Deferred`
declares storage read/write access to the same target, preserves emissive while
adding lighting, and overwrites empty pixels with the sky. This is a deliberate
read-modify-write dependency and its bandwidth is part of the measurements.

The standalone sky pass is removed only after the deferred lighting path has
visual parity. ADR-018's feedback copy is retained exactly in purpose: the
immutable pre-transmission image is the source, scene color is the destination,
and transmission/blend composite only after the copy.

## 4. Visibility and GPU submission contracts

### 4.1 Candidate and visible rows

The cull pass cannot infer a draw from `VkrInstanceDataGPU` alone. The shared
tables are:

| Struct | Contents and ownership |
|---|---|
| `VkrGpuGeometryRow` | Megabuffer vertex address/base, global index base, vertex layout/stride, and publication generation; immutable for that generation |
| `VkrGpuCandidateDrawRow` | Source instance × submesh draw data and conservative bounds; populated in a per-frame-slot bounded array |
| `VkrGpuVisibleDrawRow` | Compact geometry/material/instance/index-range/flags row; GPU-written and valid through submit completion |

The frontend or packet build produces one candidate row for each prospective
instance × submesh draw:

| Field | Meaning |
|---|---|
| `geometry_index` | Row containing the megabuffer vertex/index base |
| `material_index` | Immutable material-table row |
| `instance_index` | Model/object row |
| `first_index`, `index_count`, `vertex_offset` | Global u32 index-buffer draw range |
| `state_bucket` | Opaque/cutout and single-/double-sided pipeline bucket |
| `local_bounding_sphere` | Conservative submesh bound; culling transforms its center and scales radius by the maximum model-basis length |

`Cull.Draws` consumes the bounded candidate array and writes compacted
`VkrGpuVisibleDrawRow` records containing the geometry, material, instance,
index range, and flags needed by both raster and resolve. Bounds need not be
copied to the visible row.

#### Multi-view culling

`Cull.Draws` is defined over a **view set**, not a single camera. A view is a
frustum plus the output partition its survivors compact into. The camera is view
0; each shadow cascade is a further view. One candidate array is tested against
every view in the set. P17 replaced the earlier CPU contract, in which
`vkr_visibility_classify()` tested the camera and shadow frusta together, with
this backend-owned GPU classification.

This matters for retirement, not only for the camera path. Keeping CPU shadow
visibility and draw merging would have kept the old per-draw machinery alive
through P21 and violated §11.1's single-owner requirement. Cascades therefore
became the natural second GPU consumer: same instances, same candidate rows,
different frusta, and shadow state buckets that are a subset of the
opaque/cutout buckets in §4.2.

Per-view differences that the contract must carry:

- Each view owns its own visible-row range, indirect argument range, and count.
  Capacity and overflow policy in §5.2 apply per view, not once globally.
- Shadow views need no material resolve, so their visible rows are consumed only
  by raster.
- The camera view may apply HZB occlusion; shadow views use frustum culling
  only. Occlusion culling against a shadow-space depth pyramid is not in scope.

P17 implements this. Until then, shadow cascades stay on the CPU path, which is
legal for every phase before P20.

The input contract is triangle-list topology with `uint32_t` indices. Other
topologies/index widths must be normalized during publication or remain on the
legacy forward path before P21; `first_index + primitive_id * 3` is invalid
otherwise. P21 is blocked until no opaque or transmissive production candidate
depends on that escape path.

All shared C/shader structs are pinned by `_Static_assert`, the declarative GPU
ABI manifest, and backend reflection where supported. Publication of a table
generation and retirement of its predecessor are completion-gated.

### 4.2 State buckets

The minimum compacted buckets are:

1. Opaque, single-sided.
2. Opaque, double-sided.
3. Alpha cutout, single-sided.
4. Alpha cutout, double-sided.

This makes cull mode and cutout shader selection legal without per-command
pipeline mutation. Opaque buckets execute before cutout buckets and use a
discard-free visibility fragment; opaque shadow buckets use a depth-only
pipeline. Cutout buckets retain alpha testing, so their exact early-test
behavior remains implementation-dependent.

Before P21, additional opaque/transmission state stayed on the legacy forward
path until it had an explicit bucket or legal ICB encoding. The accepted
production state now fits these buckets; unsupported state is rejected during
publication instead of selecting another renderer topology.

### 4.3 Command and visibility encoding

The baseline emits one indexed indirect command per visible draw candidate—not
per logical object—with `instanceCount = 1` and
`baseInstance = visible_draw_index`.

The vertex stage reads base instance and passes the zero-based visible row index
to the fragment stage using a flat/nointerpolation varying. The fragment stage
does not assume direct access to base instance. It writes:

- `vbuffer.x = visible_draw_index + 1`
- `vbuffer.y = primitive_id`

Zero therefore means empty while visible row zero remains representable. Both
channels clear to zero; no `UINT_MAX` or format-ambiguous non-zero integer clear
is required.

One-command-per-candidate avoids draw-ID portability and primitive-ID
instancing ambiguity. Its command volume is still a measured tradeoff. A
future instanced encoding must carry an explicit instance discriminator and is
not adopted without evidence.

### 4.4 Indirect execution and synchronization

The graph declares compute writes to visible rows, argument buffers, and count
buffers. Raster declares shader reads of visible rows and `INDIRECT_READ` of
both arguments and counts at `DRAW_INDIRECT`. Backend lowering must produce the
corresponding memory dependency; a buffer's `INDIRECT` usage bit alone is not a
synchronization contract.

Vulkan uses indirect-count drawing when supported by the required baseline.
For Metal 4, the installed SDK exposes GPU-address indirect indexed draws,
indirect compute dispatch, ICB execution, and GPU-sourced ICB ranges. The
implementation spike therefore compares:

- GPU-address indirect calls, including a fixed-capacity stream with zero
  instance count for culled entries; and
- GPU-encoded ICB execution with a GPU-produced execution range, which is the
  candidate for collapsing CPU submission calls.

Runtime device/pipeline support remains capability-gated. If neither strategy
is correct and faster on supported Metal hardware, keep CPU direct submission
and stop the GPU-driven phase; do not hide a CPU readback/stall inside the
deferred path.

The forward-shaded GPU-culling milestone requires a table-driven forward
vertex/fragment root that reads the visible row. It isolates GPU visibility and
submission from deferred shading, not “only an indirect draw call.”

## 5. Resource ownership, capacities, and formats

### 5.1 Resource table

| Resource | Format / usage | Extent or capacity | Lifetime, clear, and reuse |
|---|---|---|---|
| Candidate rows | storage read | Fixed configured maximum candidates | Per frame slot; CPU rejects/reroutes an over-capacity frame before submit |
| Visible rows | storage read/write | At least candidate capacity | Per frame slot; completion-gated; no mathematically valid frame can overflow |
| Bucket arguments/counts | storage write + indirect read | Candidate capacity + one count per bucket | Per frame slot; completion-gated; zeroed by declared transfer/compute use |
| Opaque vbuffer | `R32G32_UINT`, color/storage/sampled as required | Render extent | Per target image; clear `{0,0}`; not reused until target-image completion |
| Opaque depth | Existing scene-depth format | Render extent | Per target image; normal-Z clear `1.0` |
| G-buffer GB0/GB1/GB2 | §6.1, storage/sampled | Render extent | Per target image; fully overwritten before read |
| Transmission vbuffer | `R32G32_UINT` | Render extent | Per target image; clear `{0,0}` |
| Transmission depth | Supported depth format | Render extent | Per target image; seeded from opaque depth by a declared copy before raster |
| HZB history | `R32_SFLOAT`, sampled/storage, full mip chain | Render extent down to 1×1 | Dedicated history ring; validity epoch and completion-gated slot reuse |
| HDR pre-transmission/scene | Existing HDR format plus required storage/transfer/color uses | Render extent | Existing per-image targets; declared graph ordering only |
| Megabuffer generation | vertex/index/storage/device address | Fixed generation capacity | Persistent; stable addresses until completion-gated generation retirement |

The graph lifetime must be `PER_IMAGE`, `PER_FRAME_SLOT`, or an equivalent
completion-aware history class as shown. A default single transient allocation
is not valid while frames overlap.

Logical opaque intermediates are 20 bytes/pixel (8-byte vbuffer plus 12-byte
G-buffer). This is not the renderer memory budget. Resource statistics and
bandwidth accounting must also include transmission vbuffer/depth, opaque
depth, HZB/history, HDR seed/read-modify-write traffic, alignment, and the
number of concurrently live target/frame/history slots.

### 5.2 Capacity and overflow policy

Capacities are fixed during frame recording. Candidate count is known before
submission; before P21, an over-capacity opaque/transmission class may use the
legacy CPU forward path for that frame and increments a named fallback metric.
It is never partially truncated. Deferred stabilization must prove that this
fallback is not exercised by the accepted workload matrix and that capacity
growth or explicit frame rejection works. After P21 there is no rendering
fallback: publication grows capacity between frames when possible, otherwise
submission fails with the named capacity error without recording a partial
frame.

Visible and argument capacity is at least accepted candidate capacity, so
compaction cannot overflow for valid input. Kernels still bounds-check writes
and set a diagnostic overflow flag. Any such flag is a correctness failure,
not permission to silently drop draws. Capacity growth occurs between frames by
publishing a new generation and retiring the old one after GPU completion.

### 5.3 Megabuffer publication

Vertex and index megabuffers are suballocated with the maximum of engine ABI
alignment and backend device-address/index-binding requirements. Published rows
remain address-stable for the lifetime of their generation. There is no
in-place compaction while a submitted frame can reference the generation.

Fragmentation, high-water marks, rejected publication, and generation
replacement are observable metrics. A replacement allocates and fills new
buffers/tables, atomically publishes them for a later frame, and retires the
old buffers only when every referencing submit is complete. Physical range
reuse follows the same completion rule.

## 6. Opaque visibility, material resolve, and lighting

### 6.1 G-buffer layout and format policy

| Target | Format | Linear channels |
|---|---|---|
| `gbuffer_albedo` | `R8G8B8A8_UNORM` | `rgb`: base color × (1 − metallic); `a`: occlusion |
| `gbuffer_specular` | `R8G8B8A8_UNORM` | `rgb`: `lerp(dielectric_specular, base_color, metallic)`; `a`: roughness |
| `gbuffer_normal` | `R16G16_SNORM` | Octahedral world normal after normal mapping |

`gbuffer_albedo` stores linear values. It is not an sRGB storage image and no
implicit transfer function is part of the contract. Every backend must validate
the exact storage-write and sampled-read format features, plus the visibility
format's color-attachment support, before enabling deferred rendering.

The 8-bit layout is a bandwidth-oriented baseline, not a fidelity assertion.
Final-color and direct-channel captures decide whether albedo/F0/roughness
quantization is acceptable. A failing channel moves to a wider linear format;
the document makes no “not observable” claim without evidence.

Metallic is folded out because lighting consumes diffuse albedo and F0. Colored
dielectric F0 is retained in RGB, consistent with ADR-017. Debug mode 6 is
consistently defined as `(max(f0), roughness, occlusion)` for the deferred path.

### 6.2 Material resolve

`GBuffer.Resolve` is a compute pass, initially 8×8 threads, with one thread per
pixel:

1. Read the encoded vbuffer. On zero, write safe G-buffer defaults plus a zero
   HDR seed, then exit.
2. Subtract one from `vbuffer.x`; resolve visible row → geometry/material/
   instance rows.
3. Fetch `i0..i2` at `first_index + primitive_id * 3` from the global u32 index
   buffer, apply `vertex_offset`, and fetch the three vertices.
4. Transform positions using the same model/view/projection and viewport
   conventions as the raster pipeline.
5. Evaluate the pixel center `(x + 0.5, y + 0.5)` through a backend-specific
   viewport-to-NDC helper that accounts for NDC Y direction. Solve
   perspective-correct barycentrics from clip-space `xyw`.
6. Evaluate the same analytic expression at adjacent pixel centers to derive UV
   gradients. Sample every material texture with explicit gradients.
7. Reconstruct front-facing from the exact backend winding/viewport convention,
   including model determinant effects. Apply the current two-sided normal flip
   and the same normal/tangent transform semantics as the forward shader.
8. Evaluate the material, octahedrally encode the mapped normal, write the three
   G-buffer targets, and seed `hdr_pre_transmission` with emissive.

The publication contract requires finite geometry and frame matrices. GPU
compaction owns table indices and draw ranges. Resolve trusts those producer
contracts instead of repeating their checks for every shaded pixel. The solver
still rejects a near-zero homogeneous solve scale or barycentric normalization
denominator, and normal reconstruction rejects degenerate interpolated inputs.
These legitimate
degeneracies increment the invalid-resolve diagnostic and write safe defaults.
Malformed GPU rows or non-finite published inputs are unsupported input, not a
recoverable per-pixel case. The retained epsilon and edge rules match across
both backends and remain visible through the dedicated debug view.

Viewport-to-NDC conversion is backend-specific. Vulkan records a positive-
height viewport and the shared projection already inverts clip-space Y, so a
top-left framebuffer pixel maps to NDC Y = -1. Visibility resolve previously
applied Metal's additional Y flip, reconstructing UVs and vertex colour from
the vertically mirrored raster location. Branded base-colour textures therefore
appeared upside down, repeated across unrelated geometry, and moved like a
screen-space projection as the camera rotated. Vulkan resolve now uses the same
unflipped framebuffer-to-NDC mapping as its deferred world-position paths, and
the analytic barycentric Y derivative uses the corresponding positive
`2 / height` step. Metal retains its own flipped mapping.

`smoke.bistro.texture_attachment_snapshot` reproduces the reported Bistro
lumberyard-sign view and captures final colour, resolved diffuse, and
barycentric/LOD diagnostics at two camera angles. The RX 6700 XT Release run
passes with distinct captures whose resolved sign and surrounding textures are
upright and remain attached to their geometry: report
`20260821T142239.074Z-003b07`, digest
`sha256:617a19207bd7f6057c95efbb8ecb78294e8e562133e14c4b59376458031dd833`.
The case has no accepted image baseline, so it is a focused visual witness, not
an automated temporal-error metric or performance result.

### 6.3 Deferred lighting

`Lighting.Deferred` reads the G-buffer, opaque depth, shadows, frame tables, and
the current `hdr_pre_transmission` seed; it declares storage read/write on that
HDR image.

Covered pixels reconstruct world position from depth and inverse view-
projection, reuse the existing BRDF/shadow/probe helpers, and add lighting to
emissive. The deferred direct-light helper consumes diffuse albedo, roughness,
and F0; `(1 − metallic)` is already folded into diffuse albedo. Empty vbuffer
pixels evaluate the sky and overwrite the zero seed.

ADR-019's world-space 384-cell bitmask grid, stable light table, exact range/cone
rejection, and local IBL probe rules remain unchanged. This decision does not
introduce clustered lighting.

Debug modes move into material resolve or deferred lighting as appropriate.
Modes must include final color, normal, diffuse, specular, mode 6 above,
visibility ID/primitive, barycentric validity, and selected texture LOD so
intermediate parity can be inspected directly.

## 7. Transmission and retained raster passes

Transmissive geometry writes its own visibility buffer and frontmost depth.
`Transmission.DepthSeed` first copies opaque depth into the transmission depth
image, after which normal depth testing rejects surfaces behind opaque geometry
and retains the nearest transmissive surface in front. The copy, image usages,
and transitions are declared graph work and included in bandwidth evidence. The
pass uses the same candidate/visible contracts and state buckets relevant to
transmission.

The baseline `Transmission.Shade` dispatch is full-screen and exits immediately
for an empty transmission vbuffer pixel. It resolves material and geometry,
samples the immutable `hdr_pre_transmission`, and composites the result into
`hdr_scene_color`. This makes the producer, source, and destination explicit
without introducing an unowned pixel list.

GPU timing showed that sparse coverage made the full-screen dispatch material,
so P19 adds a `Transmission.Compact` scan of each final
transmission vbuffer. It produces a bounded pixel list, count, overflow, and
indirect dispatch arguments while copying the layer background in the same
viewport traversal. On Metal, the scan encoder invokes the one-thread finalizer
after an explicit dispatch-to-dispatch device-visible barrier; the finalizer is
not a separate graph pass. The buffers follow the same capacity,
`INDIRECT_READ`, and completion rules as draw arguments. Raster fragments do
not append the list because they cannot know that they are the final depth
winners.

Metal P18 represents four ordered transmissive surfaces. Each peel begins from
the opaque depth seed, rejects fragments at or in front of the preceding peel,
and records the next normal-Z surface in one layer of the graph-owned visibility
and depth arrays. Four shade passes then composite layers 3 through 0 through a
separate RGBA16F ping-pong image. The bound covers the entry and exit surfaces
of the two closed double-sided transmissive meshes in the current layered
witness. It is a documented fidelity/capacity bound, not an unmeasured claim
that four layers are free or universally sufficient.

That decision is resolved **before P4**, not at P12, by the spike described in
§11. It is answerable on the current forward path at negligible cost, and it
determines only whether P18 is required or skipped — no other phase's content
depends on it:

- **Accepted.** Single-layer deferred transmission ships at P12/P13, P18 is
  skipped, and retirement proceeds.
- **Rejected.** The owner selected P18; both backends now deliver the accepted
  four-layer bound used by P20.

In neither case does transmission remain on the legacy forward path at P21.
Deferring this decision to P12 is what would turn it into a blocker, because by
then twelve phases would rest on an unvalidated fidelity assumption.

Ordinary alpha blend, world text, UI text, and UI remain direct-raster
compositing feature passes. Their narrowly owned CPU visibility/submission
machinery stays until a separate decision replaces it; it must not retain or
call the deleted opaque/transmission world renderer after P21.

## 8. HZB history and occlusion

### 8.1 Baseline history path

The existing per-target-image depth is not “previous-frame depth”; reacquiring
an image can return depth from several submissions earlier. Occlusion therefore
uses a dedicated HZB history ring with explicit previous/current logical slots.

After `VBuffer.Opaque`, authored `HZB.BuildMip.${i}` compute passes build the
current full mip chain. Every pass declares its source/destination mip slice.
The next eligible submit reads that history only after its producer dependency;
physical slot reuse is completion-gated.

The renderer uses normal Z with depth clear `1.0`. Each coarser HZB texel stores
the **maximum** child depth, so uncovered/far samples prevent aggressive
occlusion. Culling projects the conservative sphere bounds, selects a mip that
does not under-cover the screen rectangle, samples all overlapped texels, and
culls only when the candidate's nearest depth is farther than every sampled
maximum plus a documented epsilon.

The history record carries render extent, depth convention, view/projection,
and a validity epoch. Resize, camera/view/projection change, camera cut, depth-
convention change, or an occluder/world-geometry epoch change invalidates HZB
occlusion for the frame; frustum culling still runs. This deliberately
conservative baseline proves storage, synchronization, and static-view
occlusion without risking temporal false negatives.

### 8.2 Optional current-frame refinement

Moving-camera occlusion is a later measured feature, not implied by the
baseline. A two-phase current-depth scheme, if adopted, must be authored as the
following explicit graph sequence:

```text
Cull.HistoryVisible       candidates -> phase-one visible rows/args
VBuffer.HistoryVisible    phase-one indirect -> vbuffer/depth
HZB.BuildCurrentMip.${i}  current depth -> current HZB
Cull.Remaining            remaining candidates + current HZB -> phase-two args
VBuffer.NewlyVisible      phase-two indirect -> LOAD vbuffer/depth
```

It needs disjoint candidate state, correct attachment load/store behavior, and
proof that no newly visible candidate is omitted. Phase one is the intersection
of current-frustum survivors and the last submitted visible set; phase two owns
every other current-frustum survivor. If the moving-camera harness finds a
false negative, disable occlusion and retain frustum-only culling; never mask a
hole with temporal tolerance.

## 9. Picking

Picking keeps a declared GPU resolve and completion-gated readback. It does not
assume the CPU can dereference a transient visible table after submission.

For a request, `Picking.Resolve` reads the requested pixel from opaque and
transmission vbuffers/depths, selects the nearest valid normal-Z surface, looks
up the instance `object_id` on the GPU, and writes that ID to the small readback
buffer. The visible rows and instance generation remain alive until the copy is
complete.

Ordinary blend and text are absent from these vbuffers, so their feature-local
picking raster remains. Its ID/depth candidate is compared with the deferred
candidate before the final object ID is copied. UI picking remains under its
current policy.

After this path is accepted, the redundant opaque/transmission mesh picking
draws and their pipelines may be removed. `Picking.Resolve`/readback and the
blend/text picking coverage are not removed.

## 10. Graph and backend contract changes

The render-graph schema, JSON parser, realization, and both backend lowerings
change together. Required additions are:

- schema/parser condition parity for deferred, history-valid, transmission, and
  picking paths;
- executable compute descriptors with direct workgroup counts or an indirect
  argument binding/offset;
- buffer `INDIRECT_READ` access and `DRAW_INDIRECT` stage;
- image `mip_levels` and per-use base mip/layer plus counts;
- typed/validated bindings resolved by declared binding rather than use order;
- per-mip views/descriptors and format/usage capability checks;
- explicit `PER_IMAGE`, `PER_FRAME_SLOT`, and history lifetime realization;
- named capacities, overflow metrics, and completion tokens for every dynamic
  GPU-written table/buffer.

Graph JSON is rejected before recording when a binding is absent/duplicated,
the dispatch source is invalid, an indirect buffer lacks the required usage and
access, a subresource is out of range, or the selected format lacks required
features.

## 11. Migration plan

Metal is the first implementation of each renderer feature, but Vulkan mirrors
that feature before the next layer lands. Foundations that affect shared graph
or ABI contracts land on both backends together.

### Why retirement is a phase and not a cleanup

The case for deferred rendering in §1.1 is a shading-cost hypothesis. The case
for *deleting* the forward renderer is different and does not depend on it: two
whole-frame world topologies mean every material feature, shading fix, debug
channel, capacity policy, metric, and validation case is built and maintained
twice, on two backends, forever. That cost compounds with every subsequent
renderer change, and it is paid whether or not the deferred path is faster.

This is why P21 is scheduled rather than left as optional tidying. A migration
that ships the new path and never deletes the old one is the standard failure
mode for exactly this kind of work: the dual-path tax is invisible in any single
change and unbounded across all of them. P20 exists so that retirement is gated
on evidence rather than on enthusiasm, and P21 exists so that it actually
happens.

### Pre-P4 decision spike: layered transmission fidelity

The single-layer transmission question (§7) is answered **before P4**, using the
current forward path. Depth-test transmissive draws against each other so only
the frontmost shades, then capture the existing layered-transmission cases and
show the owner. This is a shader and depth-state experiment; it needs none of
the deferred pipeline.

The spike is scheduled first because it is the cheapest possible answer to the
one open question that changes the phase list, and because its cost rises with
every phase built on top of an unvalidated assumption. Its outcome selects
whether P18 is required or skipped, and nothing else.

Note on technique: for transmission, depth peeling is the correct tool and an
OIT accumulation scheme such as weighted-blended OIT is not. Transmission here
is not alpha blending — it refracts a background sample with IOR, thickness, and
Beer-Lambert attenuation, per ADR-018. A depth-weighted accumulation cannot
represent a per-layer refracted background sample at all. Peeling gives exact
per-layer ordering and repeats the existing ADR-018 topology N times, reusing
the graph `repeat` mechanism that `Shadow.Cascade.${i}` already proves. It is
both more correct here and a smaller change. OIT accumulation remains the right
tool for ordinary alpha blend, which is out of scope and not a blocker.

| Phase | Content | Backend |
|---|---|---|
| P0 | Binding-by-declaration, condition parity, executable direct compute descriptors, and removal of hot-path pass-name string dispatch | both |
| P1 | Graph-buffer realization, lifetime classes, completion-gated reuse, `INDIRECT_READ`/`DRAW_INDIRECT`, and direct/indirect compute dispatch | both |
| P2 | Required format capability checks, `mip_levels`, subresource uses, and per-mip views; no unrelated MRT/D24 work | both |
| P3 | Megabuffer generations, publication/retirement, candidate/visible ABI, fixed capacities/metrics, and a table-driven forward shader with visual parity | both |
| P4 | Capability/strategy spike, frustum cull, state-bucket compaction, and forward-shaded GPU submission | Metal |
| P5 | Frustum cull, compaction, and forward-shaded GPU submission parity | Vulkan |
| P6 | Opaque visibility buffer and debug ID/primitive views | Metal |
| P7 | Opaque visibility-buffer parity | Vulkan |
| P8 | Material resolve, explicit gradients, G-buffer, and direct channel captures | Metal |
| P9 | Material-resolve and G-buffer parity | Vulkan |
| P10 | Deferred lighting, emissive seed, sky fold, and pre-transmission HDR topology | Metal |
| P11 | Deferred-lighting and HDR-topology parity | Vulkan |
| P12 | Transmission vbuffer and full-screen fused shading; keep forward fallback | Metal |
| P13 | Transmission parity | Vulkan |
| P14 | Dedicated HZB history and conservative static-view occlusion | Metal |
| P15 | HZB-history parity | Vulkan |
| P16 | GPU picking resolve plus opaque/transmission picking-raster removal | both |
| P17 | Multi-view candidate generation: extend `Cull.Draws` to a view set, add shadow-cascade views, move cascade submission to indirect, and remove the world consumers of CPU per-submesh visibility and draw merging | both |
| P18 | Bounded layered transmission by depth peeling, if and only if the §11 fidelity spike rejected the single-layer result | Metal, then Vulkan parity |
| P19 | Optional measured work: transmission pixel compaction, material/tile classification, or current-frame two-phase HZB | one Metal slice, then Vulkan parity per accepted feature |
| P20 — accepted 2026-08-20 | Make deferred the default and stabilize it on both backends: accepted visual/GPU/performance evidence, full opaque/transmission material and state coverage, zero legacy-fallback incidence in the accepted workload matrix, capacity failure/growth coverage, and a bounded soak with the legacy topology available only for diagnosis | both |
| P21 — implemented 2026-08-20 | Completely retire the legacy forward renderer: delete its graph branch, opaque/transmission shaders and pipelines, CPU submission/fallback routes, migration flag/configuration, dual-path metrics/tests, and dead compatibility code; keep only feature-local blend/text/UI raster passes in the sole deferred topology | both |

P0-P20 were independently revertible behind the temporary legacy topology.
P20 was the explicit stability boundary rather than a short smoke run. P21
started only after the P20 evidence was owner-accepted on both backends. Runtime
rollback and per-frame rerouting no longer exist; rollback means reverting the
retirement change in source control and rebuilding.

### Historical implementation checkpoints

The dated checkpoints below record how the migration reached P20. They are not
the current feature status; the document summary and phase table above own that
status.

On 2026-08-14, P0, P1, and P2 were implemented and passed the cross-platform
CPU/build gates. P3 was implemented on both backend code paths and passed Metal
native growth/API-validation evidence; native Vulkan validation and an accepted
retained-forward pixel reference were still open evidence gates. P4, P6, P8,
P10, P12, and P14 were provisionally implemented on Metal and formed its
default topology; `VKR_DEFERRED_ENABLED=0` retained the diagnostic forward
route. Focused Metal API and shader/GPU validation passed with
34 candidates, 34 visible rows split 28/6/0/0 across the four state buckets,
zero compaction overflow, and zero rejected material-resolve pixels. The Metal
shader-validation configuration uses two completion slots and one
262,144-command ICB per camera/cascade view because Apple's validator crashes
while committing either a larger ICB allocation or more than twelve live ICBs.
Normal deferred execution uses two completion slots and one five-view ICB per
slot; diagnostic forward uses three slots. Both deferred configurations execute
the same bounded view-group ABI and command ranges.
The P10
slice seeds the graph-owned pre-transmission HDR target with emissive during
material resolve, then computes directional, punctual, shadow, IBL, ambient,
and sky lighting into that same target before feedback copy. Direct
visibility-ID, primitive-ID, diffuse, F0/roughness/occlusion, normal, emissive,
and barycentric/LOD captures remain available. P12 adds an independent
transmission candidate/compaction/ICB stream, opaque-depth seeding, a dedicated
transmission vbuffer/depth pair, direct ID/primitive captures, immutable HDR
feedback sampling, and fused material/lighting/transmission writes. P14 builds
every mip of an `R32_SFLOAT` max-depth history image through explicit graph
subresources and reads only the newest compatible completed slot. Exact
view/projection, render extent, and an opaque-world content epoch invalidate
history; a fixed normal-Z convention completes the record. The static Sponza
witness rejects 7 of 34 candidates with zero overflow/invalid resolves and
produces byte-identical final-color and depth captures to frustum-only deferred
culling.

The provisional Metal P16 slice resolves the requested opaque/transmission
visibility pixel to an object ID on the GPU, seeds feature-picking depth from
the winning deferred depth, and retains only ordinary blend/world-text/UI-text
picking raster before the existing completion-gated one-pixel readback. The
Metal P17 slice classifies one bounded candidate source over the camera and up
to four cascade views, owns disjoint per-view visible/state/ICB ranges, and
executes shadow buckets indirectly. Focused forward/deferred captures produce
byte-identical depth for all four shadow cascades; positive opaque and
transmission picking witnesses return the same requested-pixel IDs as forward,
while deferred writes no redundant full-target mesh IDs. CPU opaque/shadow
lists remain generated for the retained selector-off forward path, so full
both-backend P17 retirement is not claimed.

The provisional Windows Vulkan P5/P7/P9/P11/P13/P15/P16-P18 slices use fixed
65,536-command partitions for each of four state buckets and each camera or
cascade view. Compute classify/prefix/encode writes visible rows plus indexed
indirect commands; visibility and cutout-shadow pipelines consume them through
`vkCmdDrawIndexedIndirectCount`. Opaque `R32G32_UINT` visibility resolves into
the documented diffuse/AO, F0/roughness, octahedral-normal, emissive, and
barycentric/LOD G-buffer ABI, sampling base color, ORM, normal, and emissive
maps with analytic gradients, carrying raster `SV_IsFrontFace` in the otherwise
unused primitive-ID high bit, using a fixed opaque normal/tangent matrix
operation at the entry call site, and carrying metallic in the HDR seed alpha.
Canonical primitive-ID captures mask the internal face bit.
Deferred lighting then spells the forward model through shared `packet_punctual`
and `packet_environment` helpers that both Vulkan paths call: cascade-shadowed
directional, punctual, weighted local probes with the global-environment
remainder or the ambient fallback, the emissive seed, the
diffuse/specular/normal/unlit/material debug views, and the sky background the
authored `!deferred_enabled` skybox pass does not provide on this path. A
completion-compatible max-depth HZB ring
feeds conservative camera culling, picking resolves opaque/transmission IDs,
and four depth-peel visibility layers composite back-to-front. Invalid or
temporarily unpublished packet rows are omitted at the cold publication
boundary rather than branching in GPU hot paths. The focused state-matrix case
selects deferred with nine GPU candidates and zero rejected geometry
publications in focused Debug validation and Release executions on the target
RX 6700 XT. The final exact-tree Release child report is
`build/_artifacts/profile/manual-20260817T0747-vulkan-deferred-release/runs/0/report.json`;
the exact-tree validation-layer child report is
`build/_artifacts/profile/manual-20260817T0746-vulkan-deferred/runs/0/report.json`.
Direct
visibility captures contain IDs 1-5 for opaque and 1-4 for transmission, and
the exact-tree snapshot
`build/_artifacts/snapshot/20260817T074747.085Z-001a08/report.json` passes. Two
Release child executions against one fresh explicit cache path pass; the cold
run creates 204,376 bytes and the warm run reuses the same cache unchanged.
Vulkan P13/P18 shading is complete. Every transmission layer reconstructs its
surface through the same routine the G-buffer resolve uses, shades it with the
shared punctual/environment helpers against the unfolded base colour and
metallic factor the forward fragment shader passes, and composites refraction by
sampling the immutable layer background at a thickness-scaled screen offset,
attenuating over the travelled distance, and surrendering the Fresnel share. The
shade passes declare the cascade shadow map so the graph owns its transition.

Shading is witnessed against the retained forward path on the state-matrix
fixture. Base colour, world position, shadowing, and the lighting model agree.
The opaque resolver uses a fixed normal/tangent matrix operation selected at its
entry call site, while the independently compiled transmission entry retains its
existing fixed operation. Release SPIR-V contains `OpMatrixTimesVector` for the
opaque entry and `OpVectorTimesMatrix` for transmission, with no runtime
convention selector. Matched Release `normals` snapshots
`20260820T111902.403Z-003be6` (forward) and
`20260820T111850.699Z-000076` (deferred) reduce the whole-frame mean residual
from 0.77/255 to 0.41/255 and pixels differing by more than one 8-bit step from
12,926 to 3,077. In the opaque fixture band the named three-row X-sign residual
falls from 10,516 pixels to 667. The remaining opaque residual is 520 pixels on
double-sided secondary side faces plus 147 pixels on the shared bottom
rasterization edge. A separate 2,410-pixel transmission residual comes from the
unchanged debug composite. Exact parity remains open, but the owner accepted
this classified residual as the P20 visual threshold on 2026-08-20.

Reaching that witness required repairing asset publication, which had never
completed a texture upload on this backend. One bounded host-visible staging
chunk is in flight at a time, and buffers claimed it unconditionally, so any
scene with geometry left to publish starved texture initialization forever;
every material referencing the default diffuse or normal texture stayed
`initialization_pending`, the forward path skipped all of its draws, and the
deferred resolve sampled images that had never been uploaded. Staging now
alternates between the two classes and uses its own memory class, because a
copy source must not compete for the small device-local host-visible heap that
shader-read upload data depends on. Publication remains bounded at roughly one
chunk per two frames, so fixture warmup rises from 64 to 256 frames to witness a
fully published scene.

Vulkan records each `pass.transmission.coverage` node with an 8×8 reduction and
publishes the four counters through the existing completion-gated frame-slot
readback. The shared authored diagnostics record remains 112 bytes: the first
80 bytes are draw compaction, the next 16 are layer coverage, and Metal P19 owns
the final 16-byte compact-overflow tail. Focused Release profile
`20260820T111913.226Z-000673` reports stable 15,100 / 7,572 / 0 / 0 covered
pixels at 640×480 across 66 samples, with valid timestamps for all four coverage
passes and zero invalid resolve or overflow. Debug synchronization-validation
profile `20260820T112108.929Z-000040` passes two repetitions without a VUID,
validation error, device loss, renderer error, or fatal marker. P19's
`Transmission.Compact` remains Metal-only by ADR-028. Its conditional compact
nodes are unrealized on Vulkan, which retains the full-screen shade chain.

Windows Vulkan P20 stabilization is implemented and accepted as part of the
cross-backend P20 boundary. The shared typed whole-packet eligibility
decision now covers exact opaque and transmission capacities, independent and
combined overflow, missing/incomplete streams, shadow-only retained work, and
no-deferred-work frames. Absent `VKR_DEFERRED_ENABLED` selects deferred;
explicit zero selects the retained diagnostic forward topology. Release
state-matrix report `20260820T131540.157Z-001a38` passes two isolated
repetitions with all eight opaque/transmission state buckets represented, 46
complete GPU pass rows, nine opaque candidates, four transmission candidates,
stable 15,100 / 7,572 / 0 / 0 layer coverage, and every named fallback,
overflow, invalid-resolve, and publication-rejection assertion at zero.
Explicit-forward report `20260820T131553.539Z-0008c2` records
`legacy_forward`, deferred selection zero, and no fallback event.

The first Bistro repetition exposed a real packed-index ownership error: Vulkan
publishes one instance row per GPU candidate, while resolve roots bounded those
packed indices by the smaller retained-forward instance count. Bistro therefore
rejected 228,428 surface pixels per measured frame. Opaque and transmission
resolve roots now use their owning candidate counts. Corrected two-repetition
report `20260820T131217.900Z-000e1d` contains 254 opaque and 18 transmission
candidates, 43 complete GPU pass rows, zero invalid resolves, and zero named
fallback/overflow events. The harness now freezes a pass catalog only after
eight consecutive completed frames with the same pass topology, so a scene's
initial no-cascade packet cannot invalidate steady-state GPU timings. Its
work-volume check also excludes the completion-availability gauge
`visibility.hzb.history_valid`; actual visibility and HZB rejection counts
remain deterministic inputs.

State-matrix snapshot `20260820T131910.518Z-0040d3` passes 13 direct captures.
It exposed and verifies a second Vulkan defect: logical deferred `depth` must
copy `opaque_vbuffer_depth`, not the unused swapchain depth target. Bistro
snapshot `20260820T132214.831Z-00424a` passes final color, visibility, material,
normal, emissive, and deferred-depth captures; visual inspection confirms
populated scene geometry and high-frequency material/normal detail. Focused
Debug synchronization-validation report `20260820T132347.906Z-0016fb` passes
without a VUID, validation error, device loss, renderer error, or fatal marker.

Final Windows acceptance used an isolated clean source commit and the
repository's authoritative P20 profiles. Bistro report
`20260820T152657.594Z-0043ef`
(`sha256:b8a1ffc9346402a38455c2b4417686d17bc410995fd4d478dbb726fd4d3e521e`)
passes five of five repetitions, 256 warmup frames, 600 measured frames, stable
warmup, an exclusive GPU lane, 43 complete GPU pass rows, 254 opaque and 18
transmission candidates, and zero fallback, overflow, invalid-resolve, or
publication-rejection assertions. The required Bistro input includes the
packed `.vkt` siblings for generated specular-glossiness derivative textures;
run `tools\pack_vkt_textures.bat` before acceptance when those ignored sidecars
are absent. Enlarging the publication arena is not an accepted substitute.

State-matrix report `20260820T153320.004Z-00146d`
(`sha256:4684582b473af2a739e5cf8417a967424331e6dfd61176be53de26910211a919`)
passes five of five repetitions with all eight opaque/transmission state
buckets, 46 complete GPU pass rows, nine opaque and four transmission
candidates, stable 15,100 / 7,572 / 0 / 0 completion-gated layer coverage, and
every P20 assertion at zero. Sponza-orbit report
`20260820T153351.227Z-004172`
(`sha256:8a3b6796dfcd805bc9e69d949e64c21f1b25935e5eaee16ec99aa7aa0d64665c`)
passes five of five repetitions, 120 warmup frames, 1,500 measured frames, 27
complete GPU pass rows, 25 candidates, and every P20 assertion at zero.
Focused Debug synchronization-validation report
`20260820T153902.776Z-000afd`
(`sha256:d964615ddd8380bf9089ff39efc3a4edd3499dc57eed33a184a38b8b65cefa74`)
passes two of two repetitions without a VUID, validation error, device loss,
renderer error, or fatal marker. Fresh Sponza and packed-Bistro snapshots are
populated; the owner accepted them and the classified exact-normal residual on
2026-08-20. No accepted baseline was promoted.

These results close P20 on both backends using the previously recorded Metal
evidence and the authoritative Windows Vulkan evidence above. This Windows
rerun does not replace or claim a fresh Metal timing result. A 480-frame
complete raw-texture publication stress can exceed the bounded publication
source reserve, then the target device-local image budget; it is separate
asset-streaming/capacity follow-up rather than the bounded P20 workload. P21
remains unclaimed and requires separate authorization.

The provisional Metal P18 slice stores four explicit visibility/depth layers,
peels each layer against the prior normal-Z depth, and shades back-to-front
through a graph-declared HDR ping-pong target. Layer views share the owning
per-image allocation and are released only after that allocation's submission
has completed. The layered fullscreen capture differs from the prior
single-layer result and focused API validation passed before the host-safety
incident. The editor graph branch is reachable on Metal because packet
`editor_enabled` is copied into the graph frame. Post-fix fullscreen and editor
Release snapshots both execute all four peel/shade stages and publish the
expected final-color producer; they remain local evidence pending owner
acceptance. A later minimal serial run passed all four peel/shade stages with
both `MTL_DEBUG_LAYER=1` and `MTL_SHADER_VALIDATION=1` and no validator
diagnostic. Metal validation children must remain strictly serial; the earlier
watchdog incidents still prohibit broad or parallel validation suites.

The P19 Metal slice adds explicit bounded transmission-pixel compaction by
default. `VKR_TRANSMISSION_COMPACT_DISABLED=1` is the focused diagnostic
rollback. A viewport-sized packed
`uint32_t` coordinate list and 12-byte indirect-argument buffer are allocated
only when selected and reused serially for layers 3 through 0 in each completion
slot. Each branch-specific `Transmission.Compact.*` kernel folds the required
background copy into its 8×8 visibility scan, reserves the list once per
threadgroup, bounds every write, and records overflow per layer. The same
compute encoder then inserts a dispatch-to-dispatch device-visible barrier and
invokes a one-thread finalize kernel to write 64-thread indirect group counts;
sparse shade consumes the list. The full-screen path remains the explicit Metal
diagnostic rollback and the Vulkan production path. The harness waits
until a completion result built from the active scene before freezing its pass
catalog, and normal Metal frames retain bounded diagnostics with their owning
command slot, so coverage and overflow publish without a synchronous wait.

Timing-enabled full-screen Metal frames scan the four final transmission
visibility layers in graph-authored coverage passes, using one atomic
accumulation per 8×8 threadgroup, and publish
`visibility.transmission.covered_pixels.layer_0` through `layer_3` plus the
coverage extent only after the owning submit completes. The coverage passes do
not change the individual `Transmission.Shade.*` timestamp intervals and add no
work when timing is disabled. In local Release run
`20260814T123946.622Z-009d5e`, each layer covers 280,024 of 2,764,800 pixels,
the four shade intervals total 2.27 ms, and the four diagnostic scans total
0.93 ms. That observation triggered the compact-list implementation.

The original post-consolidation world run `20260814T132413.052Z-018388` and editor run
`20260814T132431.297Z-01848e` execute the 12-pass compact/finalize/indirect-shade
chain, report zero overflow, and produce byte-identical final and scene-color
captures to the full-screen branch. Focused serial run
`20260814T132452.412Z-000089` passes Metal API and shader validation. In matched
Release observations `20260814T131908.463Z-016868` and
`20260814T132505.612Z-0000f6`, the four compact/copy scans, finalizers, and
indirect shades total 2.710 ms versus 3.084 ms for the estimated normal
full-screen shade chain, a 12.1% reduction; total measured GPU pass time falls
15.6%. Frame wall rises 0.9% and CPU submit rises 24.2%, however. Both runs are
local, dirty, single-repetition, and warmup-unstable. That implementation
therefore remained default-off.

The 2026-08-23 follow-up first reproduced the owner-level loss, then used a
production-like Metal System Trace to isolate the fused scan/copy kernel as
95.8% texture-sample-limited and 95.3% texture-cache-limited. Replacing the
shader copy with a Metal texture-copy command was measured and rejected because
the scan/copy family rose from 1.957 to 2.182 ms. The accepted change instead
folds each one-thread finalizer into the preceding scan encoder and declares the
indirect-argument write on that graph pass, reducing the compact chain from
twelve to eight passes without changing the four-layer topology or bounded
resources.

Matched local dirty-tree Bistro GPU report
`20260822T224507.334Z-00524e` (report digest
`sha256:87f2856ff9909c0415e74a310d274bf0e0ac6bd4b449cbb7d17fb9ed32705a62`)
reports 0.061 ms cull, 0.339 ms depth seed, 1.392 ms peel raster, 0.750 ms
compact scan/finalize, and 0.663 ms sparse shade: 3.204 ms of
non-diagnostic transmission work. That is 44.7% below the original compact
5.792 ms and 47.4% below the comparable full-screen 6.095 ms. The matched
timestamp-off compact report `20260822T224903.750Z-005bcf` (digest
`sha256:dab424c7a92774f5a3a18cf56df264dddf4d92184ebb3993002e0407ced1afed`)
records frame mean/p95 20.279/25.396 ms and prepare mean 18.151 ms. The
contemporaneous full-screen control `20260822T223524.870Z-0044ea` (digest
`sha256:e91c85ce6c892881967e9c17ec0cd3782cddd2b2cc16f9f5388569bd38d66d17`)
records 20.943/26.354 ms and 19.247 ms, so frame mean improves 3.17%, frame p95
3.64%, and prepare 5.69%; submit remains 30.9% higher at 1.334 versus 1.020 ms.
Both compact child frame means agree within 0.148 ms. The comparisons have
matching lane-specific fingerprints and work volume with zero compact overflow,
but remain non-authoritative because they are local, dirty, and warmup-unstable.

World and editor final/scene-color captures are byte-identical to their
full-screen controls. A default-selector nine-channel snapshot
`20260822T225723.839Z-00661a` passes, as do the complete CPU suite, Release
build, isolated cold and warm cache reports `20260822T225833.427Z-00665d` and
`20260822T225840.103Z-00680f`, and focused serial Metal API-plus-shader
validation report `20260822T225753.614Z-0064a4` with no validator diagnostic.
Diagnostic rollback report `20260822T225857.013Z-0068e4` realizes the
full-screen shade chain. Native Vulkan validation was not available on the
macOS host; compact conditions remain false on Vulkan and the CPU graph suite
covers the shared authored declarations.

Metal P20 stabilization is the accepted default-on implementation. Deferred
eligibility is a typed whole-packet
decision. The packet publishes O(1) camera-opaque and shadow coverage summaries
alongside the candidate rows, so unrelated transmission/blend rows cannot mask
an incomplete view class and selection does not rescan the stream. Incomplete
opaque/shadow or transmission coverage and each capacity class have distinct
reason bits, never truncate work, and select the retained whole-frame forward
route only while that route is still legal. Metrics publish whether deferred
was selected plus named legacy-forward, unsupported-input, and capacity-fallback
counters. Unit tests cover the exact 262,144-row boundary, opaque and
transmission overflow, missing/incomplete streams, shadow-only retained work,
and no-deferred-work frames. Metal publication continues to normalize accepted
16-bit indices to `uint32_t`; glTF loading rejects non-triangle primitives, and
the packet path draws triangle lists only.

The dedicated `deferred_state_matrix` fixture exercises opaque and
transmissive forms of all four opaque/cutout x back/double-sided state buckets.
The Metal selector is absent-by-default deferred and accepts only an explicit
zero as diagnostic forward. Submit results carry the actual selection and
fallback reason bits; the frontend no longer infers success from configuration.
Device information, harness reports, effective configuration, and environment
fingerprints publish `world_renderer`, preventing forward and deferred evidence
from sharing an identity.

Default-selector Release profile `20260814T140447.667Z-00d3c2` passes the state
matrix over 60 measured samples; explicit-forward profile
`20260814T140526.401Z-00d516` passes 40 samples with selection and fallback both
zero, reports `legacy_forward`, and has a different environment fingerprint.
The first default Sponza orbit exposed up to 20 invalid resolve pixels when an
original triangle crossed the eye plane. Material reconstruction now solves
homogeneous screen constraints and projected face orientation without dividing
each original vertex by clip `w`. Repeated default Sponza profile
`20260814T142326.901Z-0133f8` then passes
240 samples, and default Bistro profile `20260814T142554.739Z-013f13` passes
120 samples, with deferred selected throughout and every named fallback,
compaction, resolve, transmission, and configured cascade-overflow assertion
zero. Strictly serial default-selector Metal API-plus-shader validation profile
`20260814T142535.393Z-013dd6` passes the complete state matrix; stderr contains
only the two validation-enabled notices.

Visual-audit snapshot `20260814T143401.967Z-0165f9` exposed that every
procedural 3D generator authored a zero vertex color while both world shader
families multiply material color by vertex color. This made the P20 state
matrix and its explicit-forward control black independently of renderer
topology. Procedural plane, box, cylinder, cone, torus, sphere, and arrow
vertices now use neutral white. Repeated deferred and explicit-forward state
matrix snapshots `20260814T143848.398Z-0181a6` and
`20260814T143859.180Z-01818f` contain the intended material colors and publish
the actual `world_renderer` selection.

The Metal packet debug payload now carries shadow mode 0–3 through the shared
frame constants. Deferred opaque and transmission lighting, plus retained
diagnostic forward, emit cascade selection, shadow factor, or sampled shadow
depth from the same shadow sample used by lighting. Focused deferred snapshot
`20260814T150017.827Z-006977` passes with 23 captures: opaque IDs and
primitives, all resolved material channels, HDR/final/depth, the three live
shadow debug modes, and all four raw cascade maps. The shadow outputs have
distinct digests and every replay child has empty stderr; they are no longer
aliases of ordinary final color. State-matrix snapshot
`20260814T150446.236Z-008063` additionally exercises the three modes across
opaque and four-layer transmissive surfaces; debug output bypasses refraction
and retains the sky only where no world surface exists.

After the procedural-color correction, default deferred layered-transmission
snapshots `20260814T145306.948Z-0046a0` (world) and
`20260814T145316.320Z-00448d` (editor) pass and visibly retain the additional
peeled surfaces. Explicit diagnostic-forward controls
`20260814T145343.058Z-0047c8` and `20260814T145350.952Z-004b21` also pass but
retain their shallower result; deferred-versus-forward final-color SSIM is
0.974 and 0.964 respectively. This records the intended four-layer delta for
owner review rather than treating it as exact forward parity. Final strictly
serial dual-validation profile `20260814T150227.499Z-00749a` passes the
default deferred state matrix after the debug-ABI change, with six valid
selected samples and every named fallback/overflow sample zero.

These Metal witnesses supplied the previously recorded backend half of the P20
decision. The owner accepted their visual result together with the clean,
authoritative Windows Vulkan evidence above on 2026-08-20. Exact cross-topology
normal equality is not claimed; its classified residual is the accepted P20
threshold. P20 is closed, and P21 is implemented.

### 11.1 P21 complete-retirement contract

P21 is a deletion phase, not another selector state. It is complete only when
all of the following are true:

- `deferred_enabled`, command-line/configuration equivalents, legacy graph
  conditions/resources/passes, and backend capability branches are removed;
- legacy forward opaque and transmission executors, shaders, pipelines, root
  records, bindings, caches, and creation/destruction paths have no production
  definitions or references;
- CPU opaque/transmission culling, sorting, per-draw encoding, overflow reroute,
  unsupported-material reroute, and dual-submission metrics are removed; the
  surviving candidate build owns the input once. This includes shadow cascades:
  P17 must have moved them onto the multi-view candidate path (§4.1), because
  `vkr_visibility_classify()` derives camera and shadow visibility from one
  bounds test and `vkr_draw_merge_candidates()` serves both. If cascades still
  consume CPU visibility, those functions survive P21 with a live world caller
  and this clause is unmet;
- every shipping opaque/cutout/transmission material, topology, index format,
  and render state is either represented by the deferred contracts or rejected
  before recording with an explicit unsupported-input error;
- ordinary blend, world text, UI text, UI, their picking coverage, and post
  processing retain only the narrow data and pipelines they need and do not keep
  the deleted world renderer alive through shared helpers. Shadows are not on
  this list: after P17 they are a view of the deferred submission path, not a
  retained forward feature;
- tests and harness cases stop exercising two runtime renderers and instead pin
  the deferred-only graph, rejection paths, retained feature passes, and
  cross-backend output; obsolete forward-only baselines/fixtures are archived
  or removed according to their owning policy;
- a production-reference search and linker/build evidence prove that the
  retired symbols and graph nodes are absent rather than merely unreachable.

If any opaque/transmission fallback remains reachable, including forward
transmission because its fidelity decision is unresolved, P21 is not complete
and the renderer must not be described as forward-retired.

P21 satisfies this contract in the production tree, with the publication-
boundary repair recorded below. Packet version 17 carries
GPU candidate streams plus feature-local ordinary blend and text payloads; CPU
opaque, transmission, shadow, and picking-mesh draw lists are absent. The main
graph, parser, backend executors, pipelines, shaders, metrics, and harness cases
have no selectable legacy world branch. Both backends consume the same
candidate contract. Opaque/shadow and transmission candidate streams are
independently bounded; invalid structure or capacity in either stream is
reported as an explicit pre-recording error. "Invalid structure" means the
candidate itself is malformed — a submesh index outside the geometry it names.
A candidate whose geometry or material has not finished publishing is a bounded
lifetime boundary crossed on every scene load, not invalid structure, and is
omitted for that frame instead of failing it.

The macOS retirement pass built the Release and sanitized Debug/CPU targets,
then passed the deferred-only Release state matrix
(`20260820T181514.584Z-0038f4`), the retained Metal picking witness
(`20260820T180655.173Z-0029cf`), the isolated cold/warm cache pair
(`20260820T180913.036Z-002f8c` and `20260820T180921.559Z-00320a`), and the
focused serial API-plus-shader validation matrix
(`20260820T181620.914Z-003db1`). The empty-candidate witness also passes after
making the Metal compaction-state zero upload unconditional
(`20260820T182725.701Z-004ed1`).

The native Windows Vulkan P21 rerun completed on 2026-08-21 and found a
retirement defect that the macOS pass structurally could not observe.
`vkr_bindless_vk_pack_gpu_candidates()` resolves each candidate's published
geometry and material on the CPU before recording. P21 replaced its tolerant
tail with a single `log_error` plus `false_v`, folding three conditions into
one. Two of them are not malformed input: `vkr_bindless_vk_resolve_geometry()`
returns nothing while `pending_initialization_count` is non-zero, and materials
publish on their own schedule. Publication is bounded at roughly one staging
chunk per two frames, so every frame of a cold scene load legitimately presents
candidates whose GPU rows are still uploading. After P21 each of those frames
failed command recording and then packet validation, so the Vulkan state matrix
could not render a single frame: `9/9` candidates rejected, report
`20260821T080258.646Z-002597`, zero of two repetitions. Metal never had this
path — its frame executor passes the packet candidate count straight to the
classify dispatch and lets the GPU read a geometry row table whose unpublished
rows are zero — which is exactly why a Metal-only retirement pass was green.

The fix restores the distinction the contract actually draws. A `submesh_index`
outside a *resolved* geometry is malformed candidate structure and is still an
explicit pre-recording error. An unresolved geometry or material handle is the
bounded publication boundary: those candidates are omitted for that frame, the
one-shot boundary warning is restored, and they reappear once publication
completes. `local.p21.vulkan.publication_boundary` pins this by warming only
two frames, far below the publication boundary, and requiring the frame to
execute anyway.

With that repair the Windows Vulkan P21 gate passes on the RX 6700 XT. The nine
runs below are one coherent set taken against the same final tree. All are
non-authoritative for the same two reasons — a local-only profile and a dirty
worktree carrying unrelated `.vkb` and vendor changes — so none of them is a
performance claim; they are correctness and work-volume evidence. Every one
completes both repetitions, passes every assertion, keeps work-volume rows
bit-identical across repetitions, and contains no VUID, validation error,
device-loss, renderer-error, or fatal marker.

| Gate | Case | Report | Result |
|---|---|---|---|
| Deferred-only state matrix | `local.p20.vulkan.state_matrix` | `20260821T111157.816Z-003b85` | 9 candidates, all eight opaque/transmission state buckets, 46 complete GPU passes, stable 15,100 / 7,572 / 0 / 0 layer coverage |
| Publication boundary | `local.p21.vulkan.publication_boundary` | `20260821T111202.674Z-003bfb` | 2-frame warmup leaves candidates unpublished; frames execute and omit them instead of failing |
| Empty candidates | `local.p21.vulkan.empty_candidates` | `20260821T111206.427Z-001c9e` | text-only world drives 0 opaque and 0 transmission candidates through the deferred-only graph |
| Retained picking | `local.p21.vulkan.picking_hit` | `20260821T111211.250Z-00311b` | opaque and transmission picking resolve from visibility data with no mesh re-raster |
| Cold pipeline cache | `local.p5_p18.vulkan.deferred` | `20260821T111215.866Z-0027c7` | 204,280-byte cache built cold |
| Warm pipeline cache | `local.p21.vulkan.deferred_warm` | `20260821T111220.584Z-003a25` | prewarm child writes and measured child loads the same 204,280-byte cache |
| Sponza | `local.p20.vulkan.sponza_default` | `20260821T111229.346Z-001eff` | 25 candidates over 27 complete GPU passes |
| Bistro | `local.p20.vulkan.bistro_default` | `20260821T111300.634Z-003d93` | 254 opaque plus 18 transmission candidates over 43 complete GPU passes; all four peel layers populated at 33,140 / 1,545 / 1,002 / 837 |
| Debug synchronization validation | `local.p20.vulkan.state_matrix_validation` | `20260821T111524.783Z-003ffe` | two repetitions with the debug messenger active and no marker |

The state matrix, Sponza, and Bistro work volumes reproduce the accepted P20
Windows numbers exactly. The post-deletion pipeline cache is 204,280 bytes,
down from the 217,752 bytes the pre-P21 pipeline set produced, which is
independent corroboration that the retired pipelines are gone rather than
merely unreachable. `build_release.bat`, `build.bat Debug`, and the fresh-configure
`build_test.bat` all pass; the CPU suite is green at 453 assertions with zero
failures, and `vkr_draw_batcher` appears in no build output. No
snapshot baseline was promoted.

Coverage counters are published only when `timing_enabled` is set, because the
`Transmission.Coverage.${i}` graph nodes carry that condition. A timestamp-off
profile reports all four layers and both coverage extents as invalid rather
than zero; that is a profile-selection error, not a renderer defect.

The same pass also closed the retirement residue the deletion diff had left
behind. `lib/src/renderer/vkr_draw_batch.*` — the CPU draw-key/batch structures
ADR-013 proposed — survived P21 with zero references anywhere in `lib/`, `app/`,
`tests/`, or `tools/`; it is deleted, and `vkr_draw_batcher` now appears in no
build output. The graph declared two resources no surviving pass used:
`swapchain_depth` and the editor-only `scene_depth`, the latter a viewport-sized
per-image transient that allocated for nothing. Both are removed, leaving 31
resources with no orphan and no dangling reference. Metal's `depth` capture
channel still selected those two dead names, so it captured an image the
deferred topology never writes; it now selects `opaque_vbuffer_depth`, matching
the Vulkan capture path fixed during P20. That Metal edit is not exercised here
— this host has no Metal device — so it carries Vulkan-validated reasoning and
a compile check only, and a Metal run should confirm it. Finally,
`local.p5_p18.vulkan.deferred` asserted
`visibility.transmission.gpu_visible.count` with `stat: max` against a `min`
limit, which the harness resolves as max-over-samples ≥ 1; it now reads
`stat: min` as intended.

Swapchain-multiplicity and picking coverage was added on the same day. Debug
validation-layer runs of the deferred-only topology pass on two-image
(`20260821T111543.460Z-002735`) and four-image (`20260821T111555.540Z-003f61`)
WSI-free targets — the reports confirm actual image counts of 2 and 4 — and on
requested-pixel picking (`20260821T111607.646Z-0035e3`), all with no VUID,
validation error, device-loss, renderer-error, or fatal marker. Windowed image
counts cannot be varied: the harness rejects `target_image_count` on windowed
cases because the WSI selects it.

The Windows resize path was repaired and exercised separately on 2026-08-21.
The platform event already wrote a latest-value resize mailbox, but no frame
boundary consumed it. `vkr_renderer_prepare_frame()` now exchanges that mailbox
before beginning a frame and forwards the requested extent to the selected
implementation. Retired Vulkan window targets also had an inverted fallback
guard: when present fences are unavailable, collection fell through before a
successor acquire-submit proved that presentation had released the retired
swapchain and its present-wait semaphore. Collection now waits for that proof;
devices with `VK_EXT_swapchain_maintenance1` continue to use each retired
target's present fences directly.

The Debug `smoke.sponza.windowed_resize` diagnostic passes two repetitions on
the RX 6700 XT, observing `320x240 -> 400x300 -> 320x240` with three swapchain
images and immediate present. Final report `20260821T124314.444Z-000821`,
digest
`sha256:280927c35ca78de4f035f6673a5631310bb84a49e23559251df74b8e09487ad0`,
contains no VUID, validation error, device loss, renderer error, or fatal
marker. It is a non-authoritative correctness diagnostic because the profile
is local-only, the tree is dirty, and its three-frame warmup is unstable.
Minimize/cancel and injected WSI-failure coverage remain open in the
architecture spec.

## 12. Evidence and acceptance gates

These gates apply to the change types and migration phases they name. The
summary and phase table record which phase gates have shipped; approval of the
design alone never satisfies a gate.

### 12.1 Gates by change type

| Change | Required evidence |
|---|---|
| Shared schema/parser/ABI | Focused unit tests, `./build_test.sh`, malformed-graph rejection cases, ABI asserts/reflection, both backend builds |
| Resource realization or command recording | Focused Metal API validation and focused Vulkan validation in the phase that changes each backend; never deferred to a final parity phase |
| New pipeline | Cold/warm pipeline-cache evidence with a fresh explicit cache path, plus backend pipeline creation validation |
| Candidate/cull/indirect path | Candidate count equals CPU source count; visible counts and bucket totals match a CPU/reference classification; no overflow; final-color parity before performance comparison |
| Visibility/resolve | Captures of ID, primitive, barycentric validity, selected LOD, normal, diffuse, F0/roughness/occlusion, emissive, and final color at matched cameras |
| Lighting/HDR topology | Exact or owner-approved threshold captures for final, direct, shadow, IBL, sky, emissive, and pre-/post-transmission targets |
| Transmission | Existing layered-transmission world/editor cases plus explicit owner decision on the single-layer delta |
| HZB | Static and moving-camera cases; zero false-negative visibility omissions; invalidation/resize/camera-cut cases; work-volume reduction reported separately |
| Picking | Opaque, cutout, transmission, ordinary blend, world/UI text, overlap-depth, resize, and no-hit cases with completion-gated readback |
| P20 deferred stabilization | Deferred is the default on both backends for the accepted harness matrix; owner-accepted visual channels; focused Metal/Vulkan validation; matched Release profiles; deterministic repetitions; complete material/state coverage; zero legacy-forward, unsupported-input, and overflow fallback events during the bounded soak; explicit capacity growth/rejection tests |
| P21 legacy-forward retirement | Focused deletion diff; zero production references to retired flags, graph nodes, executors, pipelines, shaders, CPU routes, and metrics; deferred-only graph/parser tests; complete CPU suite and both backend builds; focused native GPU validation; cold/warm cache evidence; retained blend/text/UI/picking/transmission captures; lifecycle and failure-path tests after deletion |

No snapshot/baseline update is accepted implicitly. A changed image is a defect
or an explicit owner decision with the affected channels recorded.

### 12.2 Performance evidence

Only phases with a performance hypothesis make a speed claim: P4/P5 submission,
P8/P9 resolve cost, P10/P11 complete opaque shading, P12/P13 transmission, and
P14/P15 occlusion. Use matched normal Release profiles and the repository's
required repetition/authority policy. GPU-timestamp and timestamp-off profiles
are different configurations and are not compared as if identical.

P20 does not invent a new speed claim, but retirement still requires the
already-accepted matched Release result to reproduce with deferred selected by
default and with every fallback counter at zero. P21 verifies that deletion did
not change that workload; any new performance conclusion requires its own
matched comparison.

The P21 Bistro rerun used a local profile identical to
`performance.offscreen` except for its non-authoritative dirty-worktree policy.
Report `20260820T181642.400Z-003e80`, digest
`sha256:fcdf1d5a9221d5af3a8eb2083ad662cfb575b38a085ef66fff976d6bf30dfe5d`,
completed all five repetitions and both assertions over 1,500 measured frames.
Work volume stayed at 77 commands and 6 calls per frame, with zero upload fence
waits. Frame-wall mean/p50/p95 were 7.979/7.743/11.561 ms versus
8.807/7.788/15.302 ms before P21. Both runs failed the strict warmup-stability
gate, and the post-change tree was dirty; these values are diagnostic only and
do not establish a performance improvement.

Report CPU submit time, per-pass GPU time, frame time, candidate/visible/command
counts, shaded pixels, HZB rejects, memory high-water marks, and all overflow or
fallback counters. Sponza orbit and the existing Bistro static case cover
different work distributions; neither alone authorizes a general claim.

Debug/API/shader validation runs are diagnostic and separate from baselines and
performance. Run exactly one validation-enabled Metal renderer process at a
time; never launch parallel validation children or a broad shader-validation
suite. The CPU suite alone is never evidence of legal GPU use.

## 13. Risks, fallbacks, and stop conditions

| Risk | Detection | Fallback / stop |
|---|---|---|
| Analytic barycentrics/gradients select wrong LOD or produce seams | Direct LOD/barycentric views and matched captures | Fix before lighting; retain legacy-forward materials only while provisional; block P21 while any remain |
| 8-bit G-buffer quantization is visible | Direct channel and final-color thresholds | Widen only the failing target, then remeasure bandwidth |
| GPU command strategy does not reduce CPU cost on Metal | P4 matched submit/frame timings | Keep CPU direct submission and stop the GPU-driven migration claim; do not enter P20/P21 |
| Visibility command volume exceeds benefit | Candidate/command counts and P4/P5 timings | Measure instanced encoding or keep the legacy CPU path before retirement; do not enter P20/P21 on an unaccepted result |
| Graph lifetime or indirect barrier is wrong | API validation, stress with overlapping frames, deterministic repetition | Stop at foundation; no feature phase proceeds |
| Single-layer transmission is unacceptable | Pre-P4 spike on the current forward path (§11), not P12 captures | Schedule P18 bounded depth-peeled layered transmission before P20; retirement stays on track because the spike lands before anything is built on the assumption |
| Layered transmission proves unaffordable after P18 is required | P18 peel-count GPU timings against the accepted budget | Cap peel depth and accept a documented N-layer bound, or stop before P20 with deferred opaque shipped and transmission unretired; do not enter P20/P21 |
| Shadow cascades keep CPU visibility, so P21 cannot delete CPU submission | P17 acceptance: no world caller of `vkr_visibility_classify()` or `vkr_draw_merge_candidates()` remains | Either land P17 or amend §11.1 to accept CPU shadow visibility as named residue; do not discover this during the P21 deletion diff |
| Multi-view culling regresses shadow submission cost | P17 matched Release profiles, per-view candidate/command counts | Retain CPU cascade submission and accept the §11.1 amendment; do not enter P21 on an unaccepted result |
| HZB produces any false-negative omission | Visibility-reference comparison on motion/invalidation cases | Disable HZB and retain frustum-only GPU culling |
| Material resolve plus G-buffer loses to forward shading | Complete P8–P11 GPU/frame evidence | Try a measured fused visibility-shading variant or keep the legacy forward renderer; do not enter P20/P21 |
| Full-screen sparse transmission dispatch is material | Coverage, overflow, pass timing, and owner frame wall | Metal defaults to the measured compact-list path; retain the full-screen diagnostic rollback and re-evaluate on regressions or materially denser coverage |
| In-flight memory exceeds the target budget | Graph resource stats including multiplicity/history | Reduce formats/history or stop; “20 B/pixel” is not a budget defense |

Classic rasterized G-buffer deferred remains the simpler fallback if analytic
surface reconstruction cannot meet correctness or cost. Direct fused
visibility shading remains a measured fallback if materializing the G-buffer is
the losing bandwidth term. Neither alternative is categorically rejected
before evidence. These are pre-retirement design alternatives; restoring a
deleted legacy forward renderer after P21 requires a new decision and new
evidence rather than a hidden runtime switch.

The baseline has no separate depth prepass because visibility raster already
provides depth with minimal shading. A prepass may be added only if matched
measurement, including the doubled geometry work, supports it.

## 14. Documentation maintenance by phase

| Document | Phase-gated update |
|---|---|
| `docs/architecture/renderer-architecture-spec.md` | Keep compute, indirect, and G-buffer status aligned with shipped phases; record the sole deferred topology and deletion boundary when P21 ships |
| `docs/tooling/renderer-harness-and-metrics-spec.md` | Keep the G-buffer and normals-attachment description aligned with shipped P8/P9 behavior |
| ADR-018 | Record the P12/P13 transmission routing and the accepted P18 four-layer peel bound |
| ADR-013 (shadow submission) | Recorded: ADR-013 names P17 for shadow-cascade visibility/submission and P21 for opaque, cutout, and transmission, and records that P21 deleted `vkr_draw_batch.*` |
| ADR-019 | Keep light assignment unchanged; amend an instance-ABI consequence only if the selected candidate design actually changes it |
| ADR-013 | Supersede opaque/transmission CPU submission only after P21; retain it for feature-local blend/text/UI |
| ADR-002 | Retain the exercised P1 graph-buffer and indirect-synchronization contract |
| ADR-022 / ADR-024 | Extend the shared GPU ABI tables that actually land |
| Earlier megabuffer/MDI proposals | Archived 2026-08-21: `instanced-rendering/SPEC.md`, its `opaque-compaction/SPEC.md`, and `san-miguel-obj-import-megabuffer-and-mdi-plan.md` are `superseded` under `docs/archive/` with pointers here |
| `docs/README.md`, ADR index, this specification, and ADR-028 | Keep status and purpose aligned with shipped phases; P21 must explicitly record that the legacy forward renderer was deleted |

The architecture spec remains the shipping-status authority. This spec and
ADR-028 are implemented after P21. The P19 Metal compact path is default-on
after its eight-pass consolidation; the full-screen diagnostic rollback and
Vulkan production branch preserve the same bounded four-layer topology.
