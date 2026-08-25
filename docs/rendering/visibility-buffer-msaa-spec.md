---
status: proposed
updated: 2026-08-25
authority: design
---
# Visibility-buffer anti-aliasing evaluation

**Document status:** Proposed evaluation. No AA mode is accepted and no
production AA code exists.

**Scope:** A temporal-input foundation, FSR 3.1 and MetalFX evaluation, and a
2x/4x visibility-buffer MSAA control.

**Depends on:** [ADR-028](../architecture/adr/028-gpu-driven-deferred-visibility-buffer.md)
and [the deferred visibility-buffer specification](deferred-visibility-buffer/SPEC.md).

## 1. Recommendation

Build temporal inputs first. Do not begin with the FSR SDK and do not commit the
frame contract to 4x MSAA before measurement.

The temporal foundation is reusable by FSR 3.1, MetalFX, a portable TAA resolve,
future internal render scaling, motion blur, and temporal denoisers. The current
visibility buffer gives it unusually strong surface identity for disocclusion.
The missing work is still substantial, but it is work the renderer will need if
it ever adopts reconstruction.

Keep MSAA as a separate control. A visibility buffer avoids a fully
multisampled G-buffer, but that does not make MSAA cheap by default. Correct
edge shading cannot average material attributes into one G-buffer texel.
Transparency, ordinary blend, picking, HZB, SDSM, capture, and depth seeding all
cross the sample-count boundary.

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
- current exposure and any pre-exposure value;
- reactive and transparency/composition masks;
- internal input extent and output extent;
- frame delta, jitter offset, and frame index; and
- a history-reset bit with a reason.

Do not hide previous transforms in the three reserved words of
`VkrInstanceDataGPU`. A matrix does not fit, and consuming the reservation
without a versioned ABI decision would make later geometry changes harder. Use
a separate completion-protected previous-transform table or a versioned wider
instance representation after measuring its bandwidth.

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

FSR 3.1 Native AA still requires reactive and transparency/composition masks.
The renderer already has useful producers:

- compare opaque HDR before transmission/ordinary blend with the composed HDR
  result to seed reactivity;
- mark transmission, ordinary blend, UI-like world elements, animated
  emissives, and procedural material changes explicitly where the luminance
  heuristic is insufficient; and
- composite UI and screen text after temporal reconstruction.

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
- an exposure discontinuity outside the accepted range; and
- a failed, cancelled, or skipped frame that breaks the expected history chain.

History resources need completion-safe rings. They are not ADR-029 retained
resources. `RETAINED` means one physical image can be read for many frames
without a write. Temporal history means a completed older image is read while a
new image is written, which matches the distinct `HISTORY` model.

### 3.5 Consumer choice

FSR 3.1 officially supports DX12 and Vulkan and provides Native AA. AMD's own
integration material states that Native AA has the largest quality-mode
overhead and still requires reactive and transparency/composition masks. It is
not a shortcut around temporal integration.

Metal is not an official FSR 3.1 API target. Apple7 supports MetalFX temporal
upscaling, which consumes color, depth, motion, exposure, and jitter. Evaluate
it at a scale supported by the API. Do not assume it provides a same-resolution
Native AA mode without a capability test.

If cross-backend algorithm identity is required, VKR needs a portable temporal
resolve. FSR plus MetalFX gives platform-native implementations, not identical
pixels.

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

## 5. Decision matrix

Choose temporal native AA when the target includes render scaling, subpixel
detail, and specular stability, and the project can own motion/mask quality.
Choose MSAA when static and geometric sharpness dominates, temporal artifacts
are unacceptable, and measured edge plus transparency cost fits the budget.

A hybrid mode is possible, but it multiplies memory and temporal complexity.
Do not plan one before either standalone path has evidence.

The likely VKR outcome is temporal as the primary mode and one-sample rendering
as the fallback. MSAA remains worth a control implementation because it gives a
sharp, history-free comparison and may suit editor or capture modes. That is a
recommendation, not an accepted decision.

## 6. Phases and gates

| Phase | Work | Gate |
| --- | --- | --- |
| T0 | Previous-transform, jitter, reset, extent, depth, exposure, and mask contract | Jitter off is byte-identical; invalid previous state is explicit |
| T1 | Motion-vector and mask debug views | Static world is zero-motion after camera compensation; rigid motion has correct direction and magnitude |
| T2 | Completion-safe history and portable diagnostic resolve | Camera cuts, resize, skipped frames, and disocclusion reject history |
| T3 | FSR 3.1 Vulkan prototype | Native AA runs with valid vectors and nonempty masks; no frame generation |
| T4 | MetalFX Metal prototype at a supported scale | Same input contract, correct sign/unit conversion, explicit algorithm difference |
| M0 | One-sample-preserving graph, image, pipeline, descriptor, and capture plumbing | One-sample outputs remain byte-identical |
| M1 | 2x/4x opaque visibility plus edge classifier | Per-sample ID/depth debug capture and measured edge fraction |
| M2 | Interior path plus fused edge shading | Sample-weighted color is correct for geometry and background edges |
| M3 | Cutout alpha-to-coverage | Foliage motion clips accepted on both backends |
| M4 | Picking, SDSM, HZB, transmission, blend, and capture contract | Whole graph is correct or limitations are explicitly rejected |
| A0 | Matched quality and Release comparison | Owner selects primary mode and fallback; ADR records the choice |

Each runtime slice needs the CPU suite, the relevant focused tests, Vulkan
validation, focused Metal validation, and matched Release evidence for any cost
claim. Exact commands and artifacts belong in the implementation task, not this
proposal.

## 7. Primary references

- [Khronos Vulkan multisample rasterization](https://docs.vulkan.org/spec/latest/chapters/primsrast.html)
- [Khronos Vulkan depth and depth resolve](https://docs.vulkan.org/guide/latest/depth.html)
- [AMD FSR 3.1 integration overview](https://gpuopen.com/presentations/2024/FidelityFX_Super_Resolution_3-1_Release-Overview_and_Integration.pdf)
- [AMD temporal super-resolution integration guide](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/docs/techniques/super-resolution-temporal.md)
- [Apple MetalFX temporal scaler](https://developer.apple.com/documentation/metalfx/mtlfxtemporalscalerbase)
- [Apple Metal feature set tables](https://developer.apple.com/metal/feature-sets/)
