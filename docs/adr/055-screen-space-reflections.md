---
status: implemented
updated: 2026-09-09
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
The spatial filter accumulates radiance multiplied by confidence and normalizes
by covered weight; all eligible taps contribute to the coverage denominator.
The temporal filter blends covered radiance and confidence with the accepted
history weight. The existing 3×3 raw neighborhood supplies a count of positive-
coverage hits. With at least three hits, history is clamped against supported
current radiance bounds, excluding misses. With one or two hits, the user approved
retaining some otherwise-clipped history radiance. For configured history weight
`w`, the unclamped component receives `u = min(w, 0.5)` and the clamped component
receives `w - u`. Both components use the same validated history coverage, so
coverage accumulation retains `w`. History already within bounds is unchanged.
The 0.5 cap applies to the newly retained unclamped component, not the history
fraction of RGB after coverage normalization. Lower configured weights remain
lower. For an empty neighborhood, the user subsequently approved retaining
depth/identity-validated history without a current-radiance clamp: coverage and
covered radiance decay by the configured weight (default 0.85) each frame.
Conditional radiance remains unchanged; rejected history clears immediately.
After 16 consecutive misses, a stationary fully supported sample retains about
7.4% coverage. This adds no reads, images or ABI fields.
The sparse policy adds no texture reads, images or ABI fields. It trades abrupt
brightness changes for possible brief trails from moving reflected objects, which
receiver depth and identity cannot detect.
Both native paths skip out-of-bounds filter taps. Trace and composite eligibility
use the same selected material roughness; normal-variance broadening changes BRDF
weights without introducing a second, raster-dependent eligibility cutoff.
SSR intersects full-resolution depth while retaining the existing half-resolution
pyramid allocation. Logical level zero reads the already-bound full-resolution
depth image; level one reads the pyramid base. Each leaf reuses its loaded depth
and accepts a hit only within that same half-open source pixel. The ray starts at
the receiver's leaf and ascends as cells exit, within the existing 48 decisions.
Thickness extends behind the recorded surface. Empty space in front must not
force descent or supply a hit; final validation allows only numerical reconstruction
roundoff ahead of the surface. The representative is the earliest point in the
accepted slab, including entry when the ray already lies inside it.
Cell crossing times are solved directly from the projected ray origin. Computing
them relative to the current traversal point introduced rounding differences that
changed the first source pixel with hierarchy traversal order.
These changes use the existing images and bindings. SSGI retains its previous
half-resolution leaf, relative crossing calculation and symmetric slab through
explicit adapters to the shared interval and cell-boundary math.
This is a compact approximation, not a sampled GGX transport estimator.

Reflection history is a coherent color/depth/stable-identity tuple in the existing
completion-safe graph history pool. Its selected producer must match the transform
history used by motion vectors, including MetalFX. The user approved reading that
matching producer while it is in flight through existing GPU synchronization:
Metal's submission event wait and Vulkan's same-queue write/read barriers. An
unrelated producer must already be complete. Tuple members and the motion
transform must have the same producer submission. Every reader extends last use;
output reuse and retirement still require completion of producers and readers.
This adds no images or waits and leaves SSGI's completed-only policy unchanged.

Projection compatibility uses the unjittered projection. Consecutive raster-jitter
phases must not invalidate accumulation. Trace retains the current jittered
projection; previous-depth reconstruction needs only the unchanged Z/W coefficients.
Motion vectors exclude raster jitter. SSR history retains the raster grid, so
reprojection starts at the half-resolution history texel center and adds motion
from the selected receiver plus the producer's previous-minus-current jitter in UV.
Using the selected full-resolution receiver UV as the origin would resample
neighboring history even under zero motion and jitter.
The user approved four bilinear history taps, each checked against its own depth
and identity before its radiance contributes. Accepted taps interpolate covered
radiance and coverage, renormalizing over valid support before applying the
configured temporal weight. Invalid taps contribute nothing; if every tap fails,
current radiance replaces history.
This costs at most nine additional history texture reads per half-resolution pixel,
with no additional image, ray or traversal step. The two formerly unused trailing
parameter floats now carry jitter UV at offsets 280/284; the record remains 288 bytes.

Radiance controls, scene/resource changes, cuts, projection and extent changes
invalidate reuse. Ordinary camera and object motion
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
retained history without increasing history storage.

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
Vulkan reflection pins the 16-byte push constants and root strides 304/32/336/368/424
bytes (the aligned composite host record is 432 bytes); each embedded SSR parameter record is 288 bytes. Metal runtime reflection
pins roots 320/320/368/432/496 bytes and compute binding zero.

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

The coverage-filter regression executes production shared functions with analytic
inputs: sparse and dense constant hits retain identical radiance, while temporal
coverage handles intermittent misses. The old temporal formula fails the same
expected result. A repeated Metal mirror/API check retains 722 hits and a maximum
0.000684 linear-HDR error. Moving Bistro captures retain finite output and unchanged
motion vectors; these captures do not establish that all visible shimmer is gone.
[The regression record](../../assets/verification/renderer-features/screen-effects-stability.txt)
retains evidence. Current-frame fallback can still lose stability when compatible
history is unavailable; the subsequent four-tap policy addresses nearest-history jumps.

A later stationary Bistro check exposed two history-selection failures: comparing
jittered projections rejected consecutive frames, and requiring CPU-observed
completion rejected the shared predecessor while it was in flight. The approved
GPU-ordering policy above fixes both. The native static regression retains the
same raw trace within FP16 noise and reduces mean covered-reflection variation
by 45.81% (0.012930 to 0.007006 across two frame transitions). It preserves all
924,963 motion values; the moving SSR+SSGI pair does too. Portable-TAA resize
passes focused Metal API validation. [The history repair record](../../assets/verification/renderer-features/ssr-temporal-history.txt)
retains exact commands, report digests, diagnostic rejection reasons and captures.
These checks establish restored accumulation, not elimination of all SSR noise.

The later jitter-corrected four-tap filter passes its affine-interpolation,
identity/depth rejection and grid-center checks. Static street history variation
falls 17.14% over eight jitter phases, below the existing 25% history-selection
regression threshold. In the user-identified cafe under-bar view, the complete
change reduces history variation only 1.46% in the selected region; it does not
establish a visible improvement. That check preceded the separately approved sparse-history policy above: a lone
current hit collapsed the radiance clamp and defeated accumulation even with valid
history. At that stage, empty neighborhoods still cleared history.
[The reprojection record](../../assets/verification/renderer-features/ssr-reprojection.txt)
retains the failed threshold, native measurements, exact commands and previews.
Matched moving captures preserve all 924,963 motion values; serial Metal API
resize and compiled shader contracts pass. Native Vulkan remains unavailable.


The subsequent surface-traversal repair passes 98 shared-math outputs, including
independent slab and path-independent crossing checks. CPU replays of the cafe
capture match a full-resolution DDA oracle's first-hit coordinates across three
jitter phases. Native Metal under-bar hits increase from about 2,000 to about
3,900 per phase, with finite static/moving output. These counts establish recovered
surface intersections, not elimination of visible shimmer. A broader cafe check
found visible counter/glassware pops: empty current neighborhoods erased
validated history between intermittent hits on a normal-varying surface. The
subsequently approved fade prevents abrupt erasure, but the eight-phase native
check still fails visual acceptance: the largest counter-region channel range
is 30 display codes, compared with 31 before the fade, and the number of
varying pixels increases as more reflection coverage survives. Independent
source geometry and approximate normal-map sampling reproduce the measured
normal variation; no normal-path correctness defect was found. The updated
shared-math check passes 106 outputs, including a 16-frame decay and immediate
rejection of invalid history. The sparse-history
relaxation alone improves under-bar history variation only 1.48%.
[The surface-traversal record](../../assets/verification/renderer-features/ssr-surface-traversal.txt)
retains the measurements, intermediate variants, commands, digests and previews.
A focused API-only resize check passes. GPU shader validation crashes in MetalTools'
report decoder with SSR both enabled and disabled; no shader result is available.
Native Vulkan remains unrun. An SSR-off repeat also reproduces the tiny FP16 motion
mismatch observed in this later batch, so strict motion equality is not claimed.


## Revisit when

Revisit traversal resolution, step count or filtering only with matched output
and frame-cost evidence. Selective planar reflections remain separate work for
surfaces that need reliable off-screen geometry.

## Code evidence

- [Portable controls and parameters](../../renderer/src/vkr_ssr.h)
- [Shared traversal and filtering](../../renderer/src/shaders/shared/ssr_kernel.slangh)
- [Authored graph](../../assets/render_graphs/main.rendergraph.json)
- [Frame controls](../../renderer/src/vkr_frame_input.h)
