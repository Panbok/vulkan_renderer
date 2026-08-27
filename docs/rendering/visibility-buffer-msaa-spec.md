---
status: partial
updated: 2026-08-27
authority: design
---
# Visibility-buffer anti-aliasing evaluation

**Document status:** Partial. The portable TAA path described by T0-T2 is
implemented on Metal and Vulkan under ADR-037. FSR, MetalFX, and every MSAA
phase remain unimplemented.

**Scope:** The shipped temporal foundation and portable TAA decision, plus the
retained 2x/4x visibility-buffer MSAA evaluation.

**Depends on:** [ADR-028](../architecture/adr/028-gpu-driven-deferred-visibility-buffer.md)
and [the deferred visibility-buffer specification](deferred-visibility-buffer/SPEC.md).

## 1. Recommendation

Retain the implemented portable same-resolution TAA path as the production
temporal consumer. It uses one backend-neutral contract and preserves
one-sample rendering as the `VKR_TAA_DISABLED=1` fallback. FSR and MetalFX are
not prerequisites for this implementation.

Rigid previous transforms, Halton jitter, exact temporal identity, own-surface
motion for opaque, transmission, and ordinary blend, authored material
reactivity, reset reasons, completion-safe history, and motion/history debug
views now ship. Manual exposure stays after scene-linear temporal history and
needs no reset. Deformation and procedural vertex motion, particles, and
dynamic material-change signals remain open.

Do not implement MSAA as part of the TAA work. The design below remains a
separate control proposal. A visibility buffer avoids a fully multisampled
G-buffer, but that does not make MSAA cheap by default. Correct edge shading
cannot average material attributes into one G-buffer texel. Transparency,
ordinary blend, picking, HZB, SDSM, capture, and depth seeding still cross the
sample-count boundary.

## 2. Why the original MSAA plan was incomplete

The current tree has sample-count types but no production multisample path:

- `VkrRgImageDesc.samples` defaults to one and the JSON parser exposes no
  authored field for it;
- `vkr_vk_create_graph_image_instance()` rejects `samples != 1`;
- Vulkan image creation and all Vulkan graphics pipelines use one sample;
- Metal pipeline creation hard-codes `rasterSampleCount = 1`;
- Metal graph image creation sets a count but still chooses single-sample
  texture types;
- production shader heaps have no multisampled texture declarations; and
- graph attachment compatibility, capture, and pipeline-cache keys assume one
  sample.

The central shading issue is more important. `GBuffer.Resolve` writes one
single-sampled albedo, specular, normal, emissive, and debug record per pixel.
At an MSAA edge, samples can belong to different primitives or the background.
Material parameters and normals cannot be averaged before BRDF evaluation. The
BRDF is nonlinear, and one G-buffer texel cannot represent two surfaces.

A correct design keeps the ordinary single-sampled G-buffer for interior pixels
and shades edge samples separately, or makes the G-buffer multisampled. The
second option gives up the main visibility-buffer memory advantage. This
document evaluates the first.

## 3. Temporal-input foundation

### 3.1 Required data

Define one backend-neutral contract before selecting a temporal consumer:

- jittered and unjittered current view-projection matrices;
- unjittered previous view-projection;
- previous object transform or an explicit invalid-previous marker for every
  visible instance;
- a two-channel motion-vector image with a documented direction and unit;
- current depth in the convention required by the consumer;
- exposure-domain metadata if a future pre-exposure path moves history out of
  scene-linear HDR;
- reactive and transparency/composition masks;
- internal input extent and output extent;
- frame delta, jitter offset, and frame index; and
- a history-reset bit with a reason.

The 80-byte `VkrInstanceDataGPU` reuses its three formerly reserved words for a
stable temporal index, generation, and owner flag. Previous matrices remain in
a separate completion-protected table, so the per-instance draw stream does not
grow and the transform lifetime stays independent of draw compaction.

Visibility identity is useful for debug and rejection, but temporal consumers
still need velocity. For rigid geometry, reconstruct the current surface from
the visibility record and project the same local position through the previous
model and camera transforms. New, destroyed, teleported, or unpublished objects
must emit invalid history rather than a large fabricated vector.

### 3.2 Jitter and derivatives

Apply camera jitter to camera-visible opaque, cutout, transmission, and ordinary
blend rendering. Keep culling conservative against an unjittered frustum. Do not
jitter shadow-map projections merely because the camera projection is jittered.

The visibility resolver currently reconstructs at pixel center. Temporal jitter
changes the raster sample position, so analytic barycentrics and texture
gradients must remain consistent with the jittered raster. Keep unjittered
matrices for motion and culling, and use the exact jittered transform for
surface reconstruction.

### 3.3 Masks and ordering

FSR 3.1 Native AA would still require reactive and
transparency/composition masks. The current portable resolve instead carries
authored material reactivity through transparent validity and uses bounded
composition change as a moving-camera fallback. A future FSR integration can
derive its masks from the same producers:

- compare opaque HDR before transmission/ordinary blend with the composed HDR
  result to seed reactivity;
- mark transmission, ordinary blend, UI-like world elements, animated
  emissives, and procedural material changes explicitly where the luminance
  heuristic is insufficient; and
- composite UI and screen text after temporal reconstruction and final FXAA.

Zero-filled masks may make an integration run, but they are not acceptance
evidence. Missing masks are a common cause of trails and locked transparent
details.

### 3.4 Reset and lifetime

Reset history on at least:

- camera cuts and teleports;
- projection or depth-convention changes;
- internal or output extent changes;
- renderer or scene generation changes;
- a temporal consumer or quality-mode change;
- a pre-exposure domain discontinuity that is not explicitly rescaled; and
- a failed, cancelled, or skipped frame that breaks the expected history chain.

History resources need completion-safe rings. They are not ADR-029 retained
resources. `RETAINED` means one physical image can be read for many frames
without a write. Temporal history means a completed older image is read while a
new image is written, which matches the distinct `HISTORY` model.

### 3.5 Consumer choice

ADR-037 selects a portable same-resolution TAA resolve so Metal and Vulkan share
one algorithm and packet contract. The resolve consumes current scene-linear
HDR color, own-surface rigid motion, validity, device depth, exact temporal
index/generation and primitive identity, authored material reactivity, and a
bounded moving-camera composition fallback. Moving-camera validation searches
the four metadata texels in the bilinear history-color footprint. The resulting
validity mask also filters history color, so a rejected neighboring surface
cannot enter through hardware bilinear interpolation; partial footprints
renormalize the surviving bilinear weights. The resolve reads the newest
completed history and writes a completion-safe successor; manual exposure and
output-space FXAA remain in the final draw after it.

TAA does not relax history rejection to hide unstable specular input. Deferred
shading first widens roughness from the same-draw screen-space normal footprint.
Metal and Vulkan apply that confirmed filter consistently. Later punctual
roughness, angular-variance, and inverse-square attenuation experiments were
removed because they did not establish a causal visual improvement. Interactive
moving-camera owner acceptance remains open.

FSR 3.1 and MetalFX remain possible future consumers of the same foundation,
not dependencies. FSR 3.1 officially supports DX12 and Vulkan; Metal is not an
official target. MetalFX temporal scaling is Apple-specific and does not
promise cross-backend algorithm identity. Neither integration is scheduled by
this decision.

## 4. Visibility-buffer MSAA control

### 4.1 Bounded scope

Start with 2x and 4x on `opaque_vbuffer` and
`opaque_vbuffer_depth`. Do not multisample the material G-buffer. Apple7 and
the current Metal floor support at most 4x render-pass MSAA.

At 4x, the two opaque resources consume:

| Extent | Visibility total | Depth total | Additional over 1x |
| --- | --- | --- | --- |
| 1920x1080 | 63.3 MiB | 31.6 MiB | 71.2 MiB |
| 2560x1440 | 112.5 MiB | 56.2 MiB | 126.6 MiB |
| 3840x2160 | 253.1 MiB | 126.6 MiB | 284.8 MiB |

These figures exclude per-target-image multiplication, allocator alignment,
transmission resources, edge lists, resolved depth, and any capture copy. They
are not a statement that the footprint is comfortable on a particular GPU.

### 4.2 Sample plumbing

The implementation needs all of the following before any quality claim:

1. an authored sample-count field in the graph schema and strict parser;
2. capability checks for color/depth format sample-count combinations;
3. multisampled Vulkan and Metal image/view creation;
4. sample count in graphics pipeline and pipeline-cache identity;
5. compatible color/depth attachment validation;
6. sampled multisample heap declarations for 2D and array resources;
7. capture conversion for visibility and depth samples; and
8. one-sample behavior that remains byte-identical.

The multisampled visibility image should become a sampled image for compute
loads. Do not depend on multisampled storage-image support unless the immutable
capability profile explicitly requires and validates it.

### 4.3 Exact edge classification

Load all sample identities and coverage. A full-coverage pixel whose samples
all carry the same identity can use the existing single-sample G-buffer and
lighting path. Any empty sample or differing identity is an edge pixel.

Weight the final color by sample multiplicity. Shading each distinct identity
once and averaging identities equally is wrong when one surface covers three
samples and another covers one.

An edge sample must reconstruct attributes at that sample's location, not at
pixel center. Vulkan and Metal sample-position APIs own those positions. The
current `vkr_vk_resolve_surface()` pixel-center contract must be generalized
outside the interior hot path so the one-sample path keeps its direct form.

### 4.4 Edge lighting

Recommended experimental shape:

1. classify edge pixels and append them to a fixed-capacity list;
2. run the current G-buffer and deferred-lighting path for interiors;
3. reconstruct and directly shade every covered edge sample;
4. include sky/background for uncovered samples; and
5. write the sample-weighted result into the pre-transmission HDR target before
   transmission begins.

This duplicates scheduling between G-buffer lighting and fused surface
lighting, but the existing transmission path already proves direct visibility
shading with shared lighting helpers. Keep the edge list bounded, report
overflow, and reject the frame or fall back to a documented safe mode. Never
silently leave unshaded edge pixels.

Measure the edge fraction before optimizing the list. Foliage with
alpha-to-coverage can make it much larger than clean-geometry rules of thumb.

### 4.5 Depth consumers

VKR uses normal-Z and builds its conservative HZB with maximum reduction. The
multisample base must therefore use the maximum depth across samples, not the
minimum. Minimum depth would advertise an occluder as nearer than its least
occluding covered sample and can cause false-negative visibility.

Other consumers need their own rule:

- SDSM scans every occupied sample and reduces the occupied minimum and maximum.
- Picking chooses a deterministic covered surface using depth and feature
  priority. Sample 0 is arbitrary and can return empty at an edge.
- A single-sampled resolved depth for ordinary blend cannot preserve opaque
  coverage. It is an approximation, not an exact MSAA continuation.
- The four transmission depth seeds cannot plain-copy multisample depth.

The last two points are the hard boundary. Vulkan does not generally permit a
single-sampled HDR color attachment with a multisampled depth attachment without
an additional mixed-sample capability. Resolving depth before ordinary blend
loses per-sample coverage. Correct transparent-edge continuation therefore
requires either multisampled transparency/depth, a coverage-aware composite, or
a named quality limitation.

Do not claim shipping MSAA until transmission and ordinary blend choose one of
those contracts. A prototype may disable them to measure opaque edge cost, but
that result is not whole-frame quality evidence.

### 4.6 Alpha-to-coverage

Alpha-to-coverage derives a sample mask from the fragment output alpha. It is
not equivalent to the current binary cutoff, and the mapping is
implementation-dependent. Keep opaque and cutout pipeline state separate.

For cutout:

- sample base-color alpha in the visibility fragment;
- define how the authored cutoff remaps alpha into coverage;
- avoid discarding before alpha-to-coverage can act;
- keep shadow alpha policy independent; and
- compare thin foliage at motion and distance, not only in a still.

If cross-backend coverage differs visibly, a deterministic shader sample mask
is the fallback, subject to sample-position and performance evidence.

## 5. Decision

ADR-037 accepts portable same-resolution TAA as the production path and
one-sample passthrough as its fallback. The choice prioritizes subpixel and
specular stability, exact visibility identity, and a shared Metal/Vulkan
algorithm.

MSAA remains an unimplemented comparison proposal. It may be reconsidered for a
concrete editor, capture, or history-free quality requirement, but is not part
of the accepted TAA implementation. A hybrid mode is not planned.

## 6. Phases and gates

| Phase | Status | Work | Gate |
| --- | --- | --- | --- |
| T0 | Implemented | Previous-transform, jitter, reset, extent, depth, scene-linear history-domain, and reactivity contract | Focused temporal contract tests cover sequences, reset reasons, cuts, and disabled mode |
| T1 | Implemented, partial evidence | Motion-vector and history debug views | Static capture shows valid zero motion; slow motion shows nonzero camera motion and accepted history; transmission and ordinary-blend fixtures show their own rigid motion; deformation remains open |
| T2 | Implemented | Completion-safe history, portable resolve, and final-draw FXAA | CPU/shader gates, Vulkan capture/synchronization validation, and exclusive native Apple M1 Pro Metal API/GPU shader validation pass |
| T3 | Not scheduled | FSR 3.1 Vulkan prototype | Native AA runs with valid vectors and nonempty masks; no frame generation |
| T4 | Not scheduled | MetalFX Metal prototype at a supported scale | Same input contract, correct sign/unit conversion, explicit algorithm difference |
| M0 | Unstarted | One-sample-preserving graph, image, pipeline, descriptor, and capture plumbing | One-sample outputs remain byte-identical |
| M1 | Unstarted | 2x/4x opaque visibility plus edge classifier | Per-sample ID/depth debug capture and measured edge fraction |
| M2 | Unstarted | Interior path plus fused edge shading | Sample-weighted color is correct for geometry and background edges |
| M3 | Unstarted | Cutout alpha-to-coverage | Foliage motion clips accepted on both backends |
| M4 | Unstarted | Picking, SDSM, HZB, transmission, blend, and capture contract | Whole graph is correct or limitations are explicitly rejected |
| A0 | Partial | Matched quality and Release comparison | Owner acceptance and broader quality/performance evidence remain open |

Each runtime slice needs the CPU suite, relevant focused tests, backend
validation, and matched Release evidence for any cost claim. ADR-037 records
the portable TAA decision and its current evidence.

## 7. Primary references

- [Khronos Vulkan multisample rasterization](https://docs.vulkan.org/spec/latest/chapters/primsrast.html)
- [Khronos Vulkan depth and depth resolve](https://docs.vulkan.org/guide/latest/depth.html)
- [AMD FSR 3.1 integration overview](https://gpuopen.com/presentations/2024/FidelityFX_Super_Resolution_3-1_Release-Overview_and_Integration.pdf)
- [AMD temporal super-resolution integration guide](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/docs/techniques/super-resolution-temporal.md)
- [Apple MetalFX temporal scaler](https://developer.apple.com/documentation/metalfx/mtlfxtemporalscalerbase)
- [Apple Metal feature set tables](https://developer.apple.com/metal/feature-sets/)
