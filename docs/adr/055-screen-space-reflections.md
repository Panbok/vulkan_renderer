---
status: implemented
updated: 2026-09-08
authority: adr
---

# ADR-055: Opaque screen-space reflections

## Status

Accepted. Implemented on both packet backends; native Vulkan evidence remains unavailable.

## Context

Saved scene probes supply stable specular lighting outside the current view.
Opaque screen-space reflections can supply visible local geometry and motion,
while retaining those probes where the screen cannot establish a hit.

The existing culling HZB is prior-frame history and is skipped during raster
jitter. GTAO's depth chain is conditional and does not establish the conservative
hierarchy needed for reflection traversal.

## Decision

SSR is an optional frame control on opaque surfaces with perceptual roughness
at most 0.6. It runs at half width and height, with at most 48 hierarchy decisions
per ray. A dedicated current-frame R32F pyramid stores minimum positive view
depth. Zero means uncovered. Reductions retain odd source rows and columns;
the last output cell owns the remaining source pixels.

The base pass also records the nearest full-resolution receiver pixel, with
stable top-left tie breaking. Trace and temporal filtering use that same receiver.
The projected ray uses homogeneous interpolation, viewport clipping and cell
entry/exit depth intervals. Hit coverage, depth thickness and facing checks
reject unsupported intersections. Depth thickness is a binary hit tolerance; its
residual is not fractional visibility. Rays use a separate small origin bias.

Trace reads the completed opaque HDR image before transmission. It writes
incoming radiance and confidence into a half-resolution RGBA16F image. Roughness
filtering is bounded: minimum-roughness mirrors retain one source sample; rougher
receivers use up to five nearby source samples and a 3×3 depth/normal-aware filter.
This is a compact approximation, not a sampled GGX transport estimator.

Reflection history is a coherent color/depth/stable-identity tuple in the existing
completion-safe graph history pool. Its selected producer must match the transform
history used by motion vectors. Radiance controls, scene/resource changes, cuts,
projection and extent changes invalidate reuse. Ordinary camera and object motion
use motion, depth and identity rejection. History remains independent of final
TAA; disabling TAA must not introduce raster jitter or disable reflection history.

Composite reads the current filtered reflection and replaces its covered fraction
of existing probe/global specular, using the same material energy and visibility
weights. Misses preserve the original HDR value. The composite reads and writes
only its own full-resolution HDR pixel, after trace finishes sampling that image.
It runs before the transmission background pyramid, so glass sees the result.

The graph owns the depth pyramid, receiver pixels and raw reflection as transient
per-image resources. It owns the three history images and reuses them only after
GPU completion. There is no duplicate resolved-reflection image: composite reads
the current history output directly.

Coated pixels use [ADR-062](062-layered-clearcoat.md)'s coat-priority policy:
trace the coat normal/roughness with the existing ray and replace only coat
environment specular. The base retains probe/global reflections. Uncoated pixels
retain base SSR. [ADR-063](063-charlie-sheen.md) attenuates that base specular
weight by sheen allocation without another ray or history. Material/texture
radiance revisions invalidate incompatible
completed history without increasing history storage.

## Consequences

Off-screen, occluded and unsupported hits retain probe lighting. The bounded
traversal can miss intersections; it does not replace scene geometry or probes.
Coarse reflection filtering loses detail on rough surfaces. The additional graph
images and passes exist only while SSR is enabled.

The frame input adds `ssr_enabled` in version 35. Deterministic harness cases
opt in explicitly; capture-summary version 10 preserves older summaries with
SSR disabled. The shared SSR parameter record is 288 bytes. Native roots and
compiled layout checks remain backend-owned under
[ADR-044](044-shader-cross-backend-contract.md).

## Alternatives considered

Reusing culling HZB or GTAO depth would couple reflection correctness to unrelated
features. Adding SSR over fully evaluated probe specular would count reflected
energy twice. Sampling HDR while modifying it would create cross-pixel feedback.
A second full-resolution composite target is unnecessary because each composite
invocation only reads and writes its own HDR pixel.

## Evidence and remaining checks

The Release wrapper compiles both production shader paths. A Slang-to-C++
execution probe checks perspective/orthographic depth, within-cell intersections,
odd-tail ownership, binary slab acceptance and temporal rejection. Compiled
Vulkan reflection pins the 16-byte push constants and root sizes 304/32/320/368/416
bytes; each embedded SSR parameter record is 288 bytes. Metal runtime reflection
pins roots 320/320/352/416/464 bytes and compute binding zero.

On Metal, the 321×241 planar-mirror fixture produces 722 half-resolution hits.
Fifteen interior pixels differ from the analytic reflected emitter by at most
0.000684 in linear HDR. Misses preserve the existing image, and a focused Metal
API-validation run passes. Hidden-window resize captures exercise the intermediate
514×386 image with 257×193 SSR; a separate capture-free round trip renders at the
restored 642×482 extent with all SSR passes active.

The 1280×720 Bistro captures exercise TAA and reflection history. All HDR values
are finite. One local Release timing observation records approximately 2.19 ms
summed mean GPU time for the SSR passes, including 1.27 ms trace and 0.51 ms
composite. This single-run observation is not an authoritative performance claim.
Native Vulkan execution and bilateral image comparison remain unavailable on the
current Metal host; ADR-044 therefore retains UNALIGNED status.

## Revisit when

Revisit traversal resolution, step count or filtering only with matched output
and frame-cost evidence. Selective planar reflections remain separate work for
surfaces that need reliable off-screen geometry.

## Code evidence

- [Portable controls and parameters](../../renderer/src/vkr_ssr.h)
- [Shared traversal and filtering](../../renderer/src/shaders/shared/ssr_kernel.slangh)
- [Authored graph](../../assets/render_graphs/main.rendergraph.json)
- [Frame controls](../../renderer/src/vkr_frame_input.h)
