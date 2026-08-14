---
status: partial
updated: 2026-08-14
authority: spec
---

# GPU-Driven Deferred Visibility-Buffer Renderer

The P0-P3 graph, lifetime, format, shared-ABI, and megabuffer foundations are
implemented on both backends. Provisional Metal P4, P6, P8, P10, P12, P14,
P16, P17, and P18 slices now ship behind the disabled-by-default
`VKR_DEFERRED_ENABLED` selector: GPU frustum/HZB culling and four-bucket ICB
submission, opaque and transmission visibility buffers, compute material
resolve and deferred lighting, four-layer depth-peeled transmission shading, a
completion-gated HZB history ring, requested-pixel visibility picking, and
camera-plus-cascade multi-view indirect submission. Vulkan
P5/P7/P9/P11/P13/P15 parity and the Vulkan halves of P16-P18 remain proposed.
P19 evidence plumbing now publishes completion-gated per-layer transmission
coverage beside Metal pass timestamps, but the optimization remains unselected
pending matched Release timing. A minimal serial Debug witness now passes with
Metal API and shader/GPU validation enabled and publishes valid P19 coverage;
that diagnostic is correctness evidence only, not performance evidence.
The retained GPU-driven forward path remains available for
selector-off comparison and bounded pre-P21 fallback, but does not run beside
deferred final color. `VKR_HZB_DISABLED=1` is the focused P14 diagnostic
rollback to frustum-only deferred culling.
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
- Stop and keep the selectable legacy forward path if visual parity, GPU
  correctness, work-volume equivalence, or matched Release measurements fail
  before stabilization.
- After deferred becomes the stable default on both backends, retire and delete
  the legacy forward topology, all whole-frame/material/overflow fallbacks to
  it, and the `deferred_enabled` migration flag. Runtime rollback ends there;
  source-control rollback remains available.

## 1. Intent and scope

### 1.1 Problem

The current opaque world path rasterizes and shades in one fragment invocation.
Each surviving or overdrawn fragment may evaluate base color, alpha test,
normal mapping, ORM, emissive, a directional light and shadow sample, the
ADR-019 punctual-light grid, global and local IBL, and screen-space
transmission. Submission is also CPU-driven per draw, with visibility decided
in `vkr_visibility.c` and backend draw state/root data encoded for every
survivor.

The proposed architecture separates three costs:

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
  the proposed G-buffer is written by compute and the visibility pass has one
  color attachment.

### 1.4 Reference boundary

The design follows published visibility-buffer and deferred-texturing
techniques, including analytic barycentric gradients for explicit texture LOD.
The owner's reference point was the RE Engine family. No implementation detail
of a shipped Capcom title is asserted because it is not verifiable from this
workspace.

## 2. Current implementation fit and blockers

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
| B10 | Schema/parser condition parity is incomplete | Add `deferred_enabled`, HZB-history validity, and transmission/picking conditions to both schema and parser validation |

As of 2026-08-13, B1-B6, B8, and B10 are implemented on both backend code
paths. The Metal halves of B7 and B9 are provisionally implemented by P4;
Vulkan parity remains open in P5. The macOS evidence gate exercised focused
Metal API/shader validation, GPU candidate compaction, ICB execution,
visibility-buffer rasterization, material resolve, and megabuffer growth.
Native Vulkan execution remains required on a supported Windows target because
this repository intentionally marks Vulkan initialization unsupported on
macOS.

The graph schema already supports buffer resource declarations. The gap is
realization and the missing access, dispatch, lifetime, binding, and subresource
semantics—not generic buffer syntax.

## 3. Target frame topology

The deferred and legacy forward topologies coexist behind a
`deferred_enabled` frame-info condition through deferred-default stabilization.
P21 then deletes the legacy topology and the condition. The surviving world
target is:

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
every view in the set, which is the same shape as today's CPU contract —
`vkr_visibility_classify()` already takes `shadow_frustums` and
`shadow_frustum_count` and derives camera and shadow visibility from a single
bounds test, and `VkrVisibilityStats` already reports `objects_culled_camera`
and `objects_culled_shadow` separately.

This matters for retirement, not for the camera path. If shadow cascades keep
CPU visibility and CPU draw merging, then `vkr_visibility.c`,
`vkr_draw_merge_candidates()`, and per-draw encoding all survive P21 to serve
them, and §11.1's requirement that "the surviving candidate build owns the
input once" cannot be met. Cascades are the natural second consumer: same
instances, same candidate rows, different frusta, and the shadow state buckets
are a subset of the opaque/cutout buckets already defined in §4.2 because shadow
draws need only cutout and cull-mode distinctions.

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
pipeline mutation. Opaque buckets execute before cutout buckets. Alpha test
prevents an unconditional early depth write; opaque-first improves rejection,
but the exact early-test behavior remains implementation-dependent.

Additional opaque/transmission state that cannot be represented by these four
pipelines stays on the legacy forward path until it has an explicit bucket or
legal ICB encoding. Deferred stabilization requires complete production-state
coverage; P21 cannot delete the legacy path while this fallback is reachable.

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

The solver guards non-finite clip values, near-zero `w`, a near-zero matrix
determinant, and a near-zero barycentric normalization denominator. A degenerate
or invalid triangle writes a diagnostic counter and safe defaults; it must not
produce out-of-bounds texture/index access or NaNs. The epsilon and edge rules
are shared by both backends and exercised by a dedicated debug view.

The current roughness filter uses screen derivatives of the normal-mapped
normal. The initial compute implementation may use geometric-normal gradients
only, but this is a fidelity delta that needs direct comparison. If it exceeds
the accepted threshold, use the additional offset normal-map samples or keep
the affected materials on the legacy forward path while deferred remains
provisional. Every such material must migrate or be explicitly unsupported
before P21.

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
rejection, and local IBL probe rules remain unchanged. This proposal does not
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

If GPU timing later shows that sparse coverage makes the full-screen dispatch
material, an optional `Transmission.Compact` compute pass may scan the final
transmission vbuffer and produce a bounded pixel list, count, and indirect
dispatch arguments. Those buffers require the same capacity, overflow,
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
- **Rejected.** The owner selected P18; Metal now delivers a provisional
  four-layer bound. Vulkan parity and P20 acceptance evidence remain open.

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
| P20 | Make deferred the default and stabilize it on both backends: accepted visual/GPU/performance evidence, full opaque/transmission material and state coverage, zero legacy-fallback incidence in the accepted workload matrix, capacity failure/growth coverage, and a bounded soak with the legacy topology available only for diagnosis | both |
| P21 | Completely retire the legacy forward renderer: delete its graph branch, opaque/transmission shaders and pipelines, CPU submission/fallback routes, migration flag/configuration, dual-path metrics/tests, and dead compatibility code; keep only feature-local blend/text/UI raster passes in the sole deferred topology | both |

P0-P20 are independently revertible behind the retained legacy topology. P20
is the explicit stability boundary: it is not a short smoke run and does not
delete the diagnostic comparison path. P21 starts only after the P20 evidence
is owner-accepted on both backends. Once P21 lands, runtime rollback and
per-frame rerouting no longer exist; rollback means reverting the retirement
change in source control and rebuilding.

Implementation status on 2026-08-14: P0, P1, and P2 are implemented and pass
the cross-platform CPU/build gates. P3 is implemented on both backend code
paths and passes Metal native growth/API-validation evidence; native Vulkan
validation and an accepted retained-forward pixel reference are still open
evidence gates. P4, P6, P8, P10, P12, and P14 are provisionally implemented on
Metal behind
`VKR_DEFERRED_ENABLED=1`. Focused Metal API and shader/GPU validation pass with
34 candidates, 34 visible rows split 28/6/0/0 across the four state buckets,
zero compaction overflow, and zero rejected material-resolve pixels. The Metal
shader-validation configuration uses two completion slots and one
262,144-command ICB per camera/cascade view because Apple's validator crashes
while committing either a larger ICB allocation or more than twelve live ICBs.
Normal execution retains three completion slots and one five-view ICB per slot;
both configurations execute the same bounded view-group ABI and command ranges.
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
lists remain generated for the retained selector-off forward path and Vulkan,
so full both-backend P17 retirement is not claimed. P5, P7, P9, P11, P13, P15,
and the Vulkan halves of P16/P17 have not started.

The provisional Metal P18 slice stores four explicit visibility/depth layers,
peels each layer against the prior normal-Z depth, and shades back-to-front
through a graph-declared HDR ping-pong target. Layer views share the owning
per-image allocation and are released only after that allocation's submission
has completed. The layered fullscreen capture differs from the prior
single-layer result and focused API validation passed before the host-safety
incident. The editor graph branch is now reachable on Metal because packet
`editor_enabled` is copied into the graph frame; its post-fix capture remains
an open gate. A later minimal serial run passed all four peel/shade stages with
both `MTL_DEBUG_LAYER=1` and `MTL_SHADER_VALIDATION=1` and no validator
diagnostic. Metal validation children must remain strictly serial; the earlier
watchdog incidents still prohibit broad or parallel validation suites.

P19 optimization work has not started because the harness formerly froze its pass catalog from
the boot graph during scene activation and therefore discarded every
active-scene CPU and GPU pass row by name mismatch. The catalog handoff is
repaired and its allocation frame is excluded from sampling. A post-fix serial
Debug diagnostic now publishes valid coverage samples with both Metal
validators enabled, but it intentionally provides no performance evidence. The
harness aborts after a renderer prepare/submit failure and reports child/prewarm
timeouts instead of silently repeating until the parent deadline. No P19
optimization is selected without a matched Release measurement; existing
synchronized/local observations remain non-authoritative. Timing-enabled Metal
frames now scan the four
final transmission visibility layers in graph-authored coverage passes ordered
after `Transmission.Shade.*`, using one atomic accumulation per 8×8
threadgroup, and publish
`visibility.transmission.covered_pixels.layer_0` through `layer_3` plus the
coverage extent only after the owning submit completes. The coverage passes
therefore do not contaminate `Transmission.Shade.*` timestamps and add no work when
timing is disabled. These metrics are a selection prerequisite, not a P19
optimization or performance result.

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

## 12. Evidence and acceptance gates

These are future implementation gates. They are not satisfied by approving
this proposal.

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
| Full-screen sparse transmission dispatch is material | Coverage and pass timing | Add explicit `Transmission.Compact`, then remeasure |
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

## 14. Documentation consequences when phases ship

| Document | Deferred update; do not apply while only proposed |
|---|---|
| `docs/architecture/renderer-architecture-spec.md` | Update compute/indirect/G-buffer status only for shipped phases; record the sole deferred topology and deletion boundary when P21 ships |
| `docs/tooling/renderer-harness-and-metrics-spec.md` | Amend the “no G-buffer/normals attachment” statement when P8/P9 ships |
| ADR-018 | Amend transmission routing when P12/P13 ship. If the pre-P4 spike rejected single-layer and P18 ships bounded peeling, ADR-018's "Revisit When: ordered or order-independent multi-layer transmission is required" trigger has fired and the amendment must record the accepted peel bound |
| ADR-013 (shadow submission) | Supersede CPU shadow-cascade culling and merging when P17 ships; this is separate from the opaque/transmission supersession at P21 |
| ADR-019 | Keep light assignment unchanged; amend an instance-ABI consequence only if the selected candidate design actually changes it |
| ADR-013 | Supersede opaque/transmission CPU submission only after P21; retain it for feature-local blend/text/UI |
| ADR-002 | Record the exercised graph-buffer/indirect synchronization behavior when P1 ships |
| ADR-022 / ADR-024 | Extend the shared GPU ABI tables that actually land |
| Earlier megabuffer/MDI proposals | Reconcile or archive only when P3/P5 implementations subsume them |
| `docs/README.md`, ADR index, this specification, and ADR-028 | Update status/purpose only when the relevant phase ships; P21 must explicitly record that the legacy forward renderer was deleted |

The architecture spec remains the shipping-status authority. This spec and
ADR-028 remain `partial` while the selector is disabled by default, Vulkan
parity is absent, the P18 acceptance evidence is incomplete, and P19 remains
unselected.
