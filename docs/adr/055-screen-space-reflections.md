---
status: implemented
updated: 2026-09-09
authority: adr
---

# ADR-055: Opaque screen-space reflections

## Status

Accepted. Full-source-resolution shaded history is implemented on both packet
backends. Release builds, compiled layouts, native Metal captures and API-resize
checks pass. Residual Bistro flicker remains; native Vulkan execution is unavailable.

## Context

Saved scene probes supply stable specular lighting outside the current view.
Opaque screen-space reflections can supply visible local geometry and motion,
while retaining those probes where the screen cannot establish a hit.

The existing culling HZB is prior-frame history and is skipped during raster
jitter. GTAO's depth chain is conditional and does not establish the conservative
hierarchy needed for reflection traversal.

## Decision

SSR is an optional frame control on opaque surfaces with perceptual roughness
at most 0.6. Trace runs at half source width and height, with at most 48 hierarchy
decisions per ray. Temporal shading and color/depth/identity history use the full
source extent, before display reconstruction or scaling. A dedicated current-frame
R32F pyramid stores minimum positive view depth. Zero means uncovered. Reductions retain odd source rows and columns;
the last output cell owns the remaining source pixels.

The base pass also records the nearest full-resolution receiver pixel, with
stable top-left tie breaking. Trace uses that receiver; temporal shading uses its
own full-resolution receiver and reconstructs incoming radiance from nearby trace
receivers. The projected ray uses homogeneous interpolation, viewport clipping and cell
entry/exit depth intervals. Hit coverage, depth thickness and facing checks
reject unsupported intersections. Depth thickness is a binary hit tolerance; its
residual is not fractional visibility. Rays use a separate small origin bias.

Trace reads the completed opaque HDR image before transmission. It writes
incoming radiance and confidence into a half-resolution RGBA16F image. Mirrors
retain one hit-source sample; rougher rays use at most five nearby source samples.
No additional ray is traced for full-resolution history.

Each source pixel gathers the half-resolution raw image once. Rough receivers use
at most nine taps in a 3×3 footprint with a continuous tent of radius 1.5 trace
pixels. Mirrors use the former composite's 2×2 footprint with tent radius 1. Each
tap is weighted against the current full-resolution receiver's depth and selected
normal. The receiver depth tolerance is `max(0.02 m, 0.05 * center_depth)`,
independent of ray thickness. SSGI retains its previous minimum through an
explicit scalar argument. The gather accumulates radiance multiplied by
confidence and normalizes by covered weight; eligible taps contribute to the
coverage denominator. The same loaded raw samples supply temporal bounds, so
there is no second 3×3 clamp-read loop.

Temporal applies its own full-resolution receiver's BRDF energy and
indirect-specular GTAO before storing or blending history. Raw RGB remains
incoming radiance; history RGB is receiver-shaded radiance. Base receivers include
sheen allocation and anisotropic GGX; coated receivers include only the selected
coat. Current raw bounds receive the same nonnegative receiver weight before
clamping shaded history. Covered shaded radiance and confidence accumulate
together. The user accepted roughness-dependent softness and short trails;
current hit count does not introduce a separate clamp-policy threshold.

Clamping relaxes continuously with selected roughness. A smoothstep from the mirror
threshold to 0.25 determines the fraction of validated history used without current
radiance bounds. Mirrors use the current bounds; roughness 0.25 and above uses
unclamped history. Both components retain the same coverage weight. Mirrors retain
their single hit-source sample and use the 2×2 reconstruction footprint above.

For configured weight `w`, roughness blend `s`, and unjittered motion magnitude `m`
in full-resolution source pixels, accepted history uses
`w + (1 - w) * (2/3) * s * (1 - saturate(m))`. Zero configured weight stays zero.
At defaults this gives 0.95 for stationary rough receivers and 0.85 at one source
pixel per frame or for mirrors. Invalid taps still contribute no history.

Empty neighborhoods fade valid covered radiance and coverage together, preserving
conditional radiance; rejected history clears immediately. At weight 0.95, history
half-life is about 13.5 rendered frames. Receiver depth, identity and motion cannot
detect a moving reflected object on a stationary bar, so retained reflections can
trail. The approved storage and execution costs are recorded below.

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
reprojection starts at the current full-resolution source-pixel center and adds
that receiver's motion plus the producer's previous-minus-current jitter in UV.
History coordinates and texel-edge bounds use the source extent, independently
of the half-resolution trace extent.
The user approved four bilinear history taps, each checked against its own depth
and identity before its radiance contributes. Accepted taps interpolate covered
radiance and coverage, renormalizing over valid support before applying the
configured temporal weight. Invalid taps contribute nothing; if every tap fails,
current radiance replaces history.
The existing four taps retain their per-tap color/depth/identity reads. Jitter UV
remains at parameter offsets 280/284; the record remains 288 bytes.

Radiance controls, scene/resource changes, cuts, projection and extent changes
invalidate reuse. Ordinary camera and object motion
use motion, depth and identity rejection. History remains independent of final
TAA; disabling TAA must not introduce raster jitter or disable reflection history.

Composite loads the same source pixel of filtered shaded history directly and applies
`HDR - coverage * current_probe + coverage * shaded_history`. The removed probe
term is recomputed at the full-resolution pixel to match the existing deferred
lighting contribution. The new history term receives no second BRDF or GTAO
multiplier. Zero coverage preserves the original HDR value. The composite reads and writes
only its own full-resolution HDR pixel, after trace finishes sampling that image.
It runs before the transmission background pyramid, so glass sees the result.
Its former bilateral history reconstruction is removed; graph bindings 8
(history depth) and 9 (trace receiver pixel) are no longer needed by composite.

The graph owns the depth pyramid, receiver pixels and raw reflection as transient
per-image resources. It owns full-source-resolution RGBA16F color, R32F depth and
RG32_UINT identity histories and reuses them only after GPU completion. There is no duplicate resolved-reflection image: composite reads
the current history output directly.

Coated pixels use [ADR-062](062-layered-clearcoat.md)'s coat-priority policy:
trace the coat normal/roughness with the existing ray and replace only coat
environment specular. The base retains probe/global reflections. Uncoated pixels
retain base SSR. [ADR-063](063-charlie-sheen.md) attenuates that base specular
weight by sheen allocation without another ray or history. Material/texture
radiance revisions invalidate incompatible
retained history without increasing history storage.

The user approved increasing the existing history images to the full source
extent. At source 1280×720 with three frame slots and five history instances,
color/depth/identity consume 87.890625 MiB instead of 21.972656 MiB: an increase
of 65.917969 MiB, within the approved 65.92 MiB. These are logical payload bytes;
they exclude backend alignment and overlap during resize. Image count, history
pool ownership, reader retirement, rays, 48 traversal decisions and five
hit-source taps remain unchanged. Temporal executes about four times as many
pixels. The Metal source bound is 80 texture reads for rough base receivers and
71 for rough coat receivers; mirrors need at most 55 and 46 respectively. Each
output writes the same three images. Reusing the raw gather for bounds removes
the old nine-read clamp loop. Temporal shading samples no probe cubemap.

`SSR.Temporal` retains graph reads 17 (albedo), 18 (GTAO, only when enabled), 19
(sheen), and 20 (anisotropy), borrowing existing frame/LUT resources. Temporal
roots remain 464 bytes on Metal and 400 on Vulkan. Metal retains its per-consumer
frame-root creation policy: three 512-byte upload cells (1536 reserved bytes,
592 added payload bytes) established by shaded history. Vulkan retains its
frame-slot frame root and the existing 32-byte temporal upload increase.
Removing composite's two unused resource bindings shrinks its root from 496 to
480 bytes on Metal and from 432 to 416 bytes on Vulkan. The shared parameter
record stays 288 bytes. Assertions and compiled reflection pin native layouts.

Old coat probe removal uses packed coat roughness and the coat-directed GTAO cone
to match deferred lighting on both backends. Bent direction is decoded against
the coat normal, including its visibility-dependent fallback blend. Shaded coat
history retains filtered roughness and evaluates its cone after orienting the coat
normal toward the view. Composite skips base normal-variance samples
on coated pixels; Vulkan's coat branch adds its previously omitted GTAO sample
while removing six unused base specular/normal/variance reads. Metal removes four
variance reads. Normalized GTAO coordinates preserve the disabled 1×1 sentinel.

Trace source sampling is linear-clamp on both backends, retaining fractional hit
positions and cone offsets. Mirrors use one sample and rough receivers at most
five. Vulkan reuses its existing renderer-owned linear-clamp sampler; its trace
root stays 336 bytes by consuming reserved padding. These corrections implement
the accepted sampling and directional-occlusion policy without new resources.

## Consequences

Off-screen, occluded and unsupported hits retain probe lighting. The bounded
traversal can miss intersections; it does not replace scene geometry or probes.
Coarse incoming-radiance reconstruction still loses detail on rough surfaces.
Full-resolution receiver shading avoids transporting one half-resolution
receiver's material, coat or mirror response across neighboring source pixels.
It does not add geometric reflection detail beyond the half-resolution rays, and
depth/normal reconstruction can still soften boundaries. Retained history can
trail through motion or lighting changes. The larger history allocation and
fourfold temporal pixel count require measured memory and frame-cost checks.
The additional graph images and passes exist only while SSR is enabled.

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
invocation only reads and writes its own HDR pixel. Half-resolution shaded history
was smaller but reconstructed material response across receiver boundaries. The
accepted design spends history memory and temporal work to shade each source
receiver while retaining the bounded half-resolution ray workload.

## Evidence and remaining checks

The fractional-source and coat-occlusion corrections pass Release shader builds,
132 shared-math outputs, eleven SSR/SSGI/deferred SPIR-V modules and the Metal
layered-material witness with partial GTAO coverage. Native Vulkan comparison
remains unavailable. The corrected MetalFX HDR capture shows residual variation
before tonemapping at the reported Bistro edge hotspots; it does not establish an
upscaler integration defect or flicker-free reflections. [The correction record](../../assets/verification/renderer-features/ssr-sampling-and-coat-occlusion.txt)
contains exact commands, digests, numeric checks and the material preview.

The following measurements describe the preceding full-resolution-history change:

Release app and editor builds, 128 shared-math outputs, all ten production
SSR/SSGI SPIR-V modules and Vulkan host syntax checks pass. Compiled Vulkan
composite stride 416 and coat/sheen/anisotropy offsets 404/408/412 match host
assertions; Metal's native 480-byte composite root passes pipeline validation.
The mirror retains 696 raw hits and improves its maximum interior HDR error to
0.000195313. Static/moving Bistro, layered materials and combined SSR/SSGI captures
have finite payloads, valid coverage and source-resolution version-4 history.
Two serial Metal API-validation resize repetitions pass.

Visual evidence is mixed. Counter-edge pixel (548,244) becomes steady and the
under-bar hotspot (904,562) improves from seven to five display codes of variation.
Other thin edges regress. A fixed 86,265-pixel reflective bar-face polygon has
470 pixels varying by more than three codes both before and after; its absolute
SSR contribution drops 7.33%. This does not establish flicker-free reflections or
a decisive whole-region stability improvement.

Matched local Release profiles at source 1025×577 record temporal GPU time
0.435→1.364 ms, composite 0.638→0.347 ms and total SSR 1.992→2.631 ms. These
non-authoritative observations have identical effective configurations and
fingerprints but unstable warmup; they are not an equivalent-output speed claim
or a locked-60-fps result. [The full-resolution record](../../assets/verification/renderer-features/ssr-full-resolution-history.txt)
contains exact commands, report digests, image metrics and retained preview/clip.

`ssr_reflection` version 4 identifies full-source-resolution shaded RGB; version 3
held half-resolution shaded RGB and `ssr_raw` remains incoming radiance at
version 2. Prior captures cannot validate the new history extent. Native Vulkan
execution and bilateral comparison remain unavailable.

### Prior half-resolution shaded-history evidence

The following results predate full-resolution history and establish its baseline,
not validation of the current implementation.

Half-resolution shaded-history validation passed Release app/harness and editor builds and 124
shared-math outputs. Ten SSR/SSGI SPIR-V modules pass validation. Compiled temporal
SPIR-V declares offsets 368/376/380/384/388/392 and `ArrayStride 400`, checked with
`spirv-dis`. That revision's root sequences were 320/320/368/464/496 bytes on Metal and
304/32/336/400/424-byte declared strides on Vulkan (the composite host record is
aligned to 432). The shared parameter record remains 288 bytes. Native Vulkan
execution and bilateral image comparison remain unavailable.

That revision's native Metal mirror capture retains 696 raw hits and 15 analytic
samples with maximum linear-HDR error 0.000684. The 1280×720 Bistro bar capture
with 96 warmup frames contains 7,090,696 finite RGBA16F tuples and confidence in
[0,1]. In its matched comparison, counter pixels with more than three display
codes of SSR-only variation decrease from 108 to 95; mean absolute reflected
display contribution changes from 0.837653 to 0.838806 codes. The original hotspots
remain within 1–2 codes. At pixels (548,244), (549,244), and (550,244), ranges
fall from 8 codes to 4, 5, and 4 respectively. The lower under-bar region instead
increases from 9 to 25 small outliers, so these results do not establish uniform stability or perfect
reflections.

A separate layered fixture with GTAO enabled and a unit-white environment contains
294,912 finite RGBA16F tuples. Its anisotropy-plus-sheen base region has 693 nonblack
raw hits and 747 nonblack history samples; its coat region has 710 in both. These
non-degenerate checks establish participation and finite output, not bilateral
lighting agreement. The moving-camera capture has 1,385,763 finite RGBA16F tuples,
confidence in [0,1], matching payload digests, and version-3 reflection metadata.
Finite moving output does not establish absence of trails. Two serial API-only
mirror-resize repetitions pass without errors; both logs confirm Metal API
validation was enabled.

Matched Release profiles use three repetitions of 120 measured frames after 60
warmup frames, with matching configuration and workload/environment fingerprints.
Summed SSR GPU means increase from 1.911372 to 2.003074 ms (+0.091703 ms).
Temporal changes from 0.309109 to 0.438030 ms and composite from 0.679913 to
0.642413 ms. Output changes intentionally; this is a measured cost tradeoff,
not an equivalent-output speedup.

That revision used `ssr_reflection` version 3 because RGB contained
receiver-shaded radiance; `ssr_raw` stays incoming radiance at version 2. The initial
mirror, bar and layered captures above preceded that metadata bump but executed
the same shaded-history shader behavior. Their provenance is recorded explicitly;
they must not be interpreted as ordinary version-2 incoming-radiance baselines.
The final self-contained layered fixture and moving captures use version 3. A combined SSR/SSGI moving capture also
passes finite-value, confidence and payload-hash checks over 2,310,726 tuples
with both features enabled and reflection capture version 3. [The shaded-history record](../../assets/verification/renderer-features/ssr-shaded-history.txt)
retains exact runs, commands, digests, numeric checks and outstanding gates.

### Prior incoming-radiance-history evidence

The evidence below predates shaded history and records the incoming-radiance
history implementation. Its raw/history comparisons and timing results must not
be read as validation of the new history meaning. Release wrappers compiled that
production shader and app/editor revision, and its shared-math probe passed 116
outputs covering depth/slab traversal, odd tails, path-independent crossings,
history reprojection/rejection, continuous rough accumulation, motion weights
and receiver filtering.

The native Metal 321×241 mirror fixture has 696 half-resolution hits; 15 interior
samples match the analytic emitter within 0.000684 linear HDR. The earlier bar
static/moving captures contain 7,090,696 and 1,385,763 finite RGBA16F tuples, with
SSR confidence in [0,1]. A moving-camera capture with SSR and SSGI enabled also
passes finite HDR, motion and reflection checks across three checkpoints. The geometric traversal replay independently agrees with
a full-resolution DDA oracle's first supported hit coordinates across three jitter
phases. [The traversal record](../../assets/verification/renderer-features/ssr-surface-traversal.txt)
retains the earlier failures, exact commands and native evidence.

In matched 1280×720 Bistro bar captures with 32 warmup frames, continuous filtering
reduces counter-region pixels with more than three display codes of SSR-only
variation from 1,260 to 383. The original hotspots fall from ranges of about 30
codes to 3. After 96 warmup frames, those hotspots stay within 1–2 codes; tightening
the receiver depth tolerance reduces remaining counter outliers from 151 to 108.
Reflected contribution is retained. Smaller curved-surface/silhouette variation
remains, including changes in receiver lighting after the stable incoming-radiance
history. These results do not establish perfect reflections or absence of trails.

A matched local Release observation measures summed SSR means of 1.89 ms versus
1.87 ms before the filtering changes, with three repetitions of 120 measured frames
after 60 warmup frames. Workload and environment fingerprints match, but output
changes intentionally and neither run is authoritative. [The rough-history record](../../assets/verification/renderer-features/ssr-rough-history.txt)
retains configuration, commands, report digests, statistics and previews.

Focused Metal API-only resize checks pass. GPU shader validation crashes inside
the MetalTools report decoder with SSR both enabled and disabled; no shader result
is available. An SSR-off repeat also reproduces a tiny FP16 motion mismatch, so
strict motion equality is not claimed for the later batch. Native Vulkan and
bilateral image comparison remain unavailable on this host; ADR-044 stays
UNALIGNED. [The earlier history](../../assets/verification/renderer-features/ssr-temporal-history.txt)
and [reprojection](../../assets/verification/renderer-features/ssr-reprojection.txt)
records retain the completion/jitter failures and unavailable checks.

## Revisit when

Revisit traversal resolution, step count or filtering only with matched output
and frame-cost evidence. Selective planar reflections remain separate work for
surfaces that need reliable off-screen geometry.

## Code evidence

- [Portable controls and parameters](../../renderer/src/vkr_ssr.h)
- [Shared traversal and filtering](../../renderer/src/shaders/shared/ssr_kernel.slangh)
- [Authored graph](../../assets/render_graphs/main.rendergraph.json)
- [Frame controls](../../renderer/src/vkr_frame_input.h)
