---
status: implemented
updated: 2026-09-09
authority: adr
---

# ADR-055: Opaque screen-space reflections

## Status

Accepted. Reflected-hit reprojection and full-source-resolution incoming-radiance
history are implemented on both packet backends. Native validation and image
comparison evidence are recorded below; native Vulkan execution is unavailable.

## Context

Saved scene probes supply stable specular lighting outside the current view.
Opaque screen-space reflections can supply visible local geometry and motion,
while retaining those probes where the screen cannot establish a hit.

The existing culling HZB is prior-frame history and is skipped during raster
jitter. GTAO's depth chain is conditional and does not establish the conservative
hierarchy needed for reflection traversal.

## Decision

SSR is optional on opaque surfaces with perceptual roughness at most 0.6.
Half-resolution rays use an independent current-frame positive-depth pyramid,
full-resolution leaves and at most 48 hierarchy decisions. The nearest covered
source pixel owns each trace, with stable top-left tie breaking and odd-extent
reductions that retain the final rows and columns. Zero depth means uncovered.
Coated pixels trace the coat; uncoated pixels trace the base. Misses retain probes.

Traversal uses homogeneous interpolation, clipped screen bounds, absolute cell
crossings and the earliest supported rear-depth slab. Thickness is a binary
intersection tolerance; a separate 1 mm normal offset separates the origin.
Leaf validation reuses its loaded depth and accepts only its half-open source
pixel, allowing numerical roundoff ahead of the surface. Mirrors sample incoming
HDR once; rough receivers use at most five linear-clamp samples at fractional hit
coordinates. SSGI retains its prior leaf/slab policies through shared adapters.

Trace writes half-resolution RGBA16F incoming radiance/coverage and RGBA32_UINT
hit metadata. The latter contains the bit patterns of hit UV and positive view
depth, plus the visible-draw row that identifies the reflected instance. Integer
storage preserves these bits without floating-point conversion. Every miss and
early return writes zero metadata. No extra ray or trace texture read is needed;
the accepted leaf already reads the hit's visible row.

Temporal runs at full source resolution. It reconstructs incoming radiance from
at most nine raw taps in a 3×3 continuous tent; mirrors use four taps with a 2×2
footprint. Depth/selected-normal bilateral weights use a 2 cm minimum receiver
depth tolerance and 5% relative tolerance. Covered weights normalize radiance;
all eligible weights normalize coverage. The same raw samples supply clamp bounds.
Temporal history stores incoming light, so history lookup does not transport the
previous receiver's material, BRDF or GTAO response.

The user approved reflected-hit reprojection to replace receiver-motion lookup.
The gather retains the trace with the largest component of weighted RGB entering
the spatial numerator, without averaging hit positions across objects. Covered
weight breaks equal-energy ties; traversal order resolves exact ties. Ranking
coverage alone can tag a lamp-dominated mixture with a dark object's geometry,
rejecting otherwise reusable lamp history as raster jitter moves the samples.
Both backends use the shared ranking helper. Two additional reads fetch that
sample's hit metadata and traced receiver's visible row. Its stable receiver identity must match the
current pixel before history may use the plane. A different receiver can still
contribute current spatial radiance, but cannot establish temporal correspondence.

Current and retained instance models transport both the receiver plane and hit
into the selected producer's camera. Point transport supports nonsingular affine
models; normal transport uses the corresponding inverse transpose. Prior transform
validity, generation and frame must match for both instances. The selected
transform buffer also owns that producer's camera view matrix, published only
after successful submission. History works with TAA disabled.

For plane point `P`, unit normal `n` and hit `H`, the virtual reflected point is
`Q = H - 2*n*dot(n,H-P)`. Projecting the current and transported prior virtual
points gives a UV motion delta. Adding this delta to the current full-resolution
pixel preserves its offset from the traced half-resolution receiver. Reusing the
current jittered projection requires the selected producer's previous-minus-current
jitter, retained at parameter offsets 280/284. The prior UV ray intersects the
transported receiver plane to determine expected receiver depth; receiver-motion
depth describes a different point and is not used.

Four bilinear history taps independently validate both receiver and reflected
instance index/generation, receiver depth, virtual depth, selected receiver normal
and UV bounds. Each depth tolerance uses the configured absolute/relative limits;
selected view-space normals must have dot product at least 0.9. Unsupported
transforms, degenerate/grazing intersections, absent hits and rejected taps use
current radiance or probes. They do not retain and fade an unverified reflection.
Curved and normal-mapped receivers remain a local tangent-plane approximation.

History RGB retention remains capped at the motion/roughness-adjusted weight,
including sparse current coverage. Defaults range from 0.85 during motion to 0.95
for stationary rough receivers. Motion is now reflected-hit motion with jitter
removed. Current bounds clamp mirrors; roughness continuously relaxes clamping,
with at most half the out-of-bounds RGB residual surviving an update. Constant
sparse radiance stays constant. The legacy empty-neighborhood decay remains in
the shared helper, but SSR no longer supplies history without a supported hit.

Composite loads filtered incoming radiance at its current source pixel, applies
that pixel's BRDF/GTAO once, and computes
`HDR - coverage * current_probe + coverage * current_shaded_reflection`.
Base SSR shares the deferred base-normal-filtered BRDF, sheen allocation and
anisotropic response with its current probe replacement. Coat SSR uses filtered
selected-normal roughness; removed coat probe light uses packed roughness and
the coat-directed GTAO cone to match deferred lighting exactly. Base probe light
remains on coated pixels. Composite writes only its own HDR pixel after trace
finishes sampling it and before transmission builds its background pyramid.

The graph owns all images and the declared transform-history read. The selected
color/geometry/identity tuple and transform buffer must share a producer submission,
frame and scene. Reading that exact producer while in flight uses Metal's existing
submission-event wait or Vulkan's same-queue write/read barriers. Every reader
extends last use; output reuse and retirement require producer and reader
completion. Scene/resource/radiance revisions, cuts, projection or extent changes
invalidate incompatible history. No extra wait or history instance is introduced.

The approved resource change at source 1280×720 is:

| Resource | Format and extent | Three frame slots / five histories | Eight frame slots / ten histories |
| --- | --- | ---: | ---: |
| New raw hit | Half-resolution RGBA32_UINT, per frame image | +10.546875 MiB | +28.125 MiB |
| Geometry history | Full-resolution R32F → RGBA32F | +52.734375 MiB | +105.468750 MiB |
| Identity history | Full-resolution RG32_UINT → RGBA32_UINT | +35.156250 MiB | +70.312500 MiB |
| Total increase | Logical image payload | **98.437500 MiB** | **203.906250 MiB** |

RGBA32F geometry stores receiver depth, virtual depth and two signed octahedral
components of the selected view-space normal. RGBA32_UINT identity stores the
receiver and reflected instance index/generation pairs. Existing full-resolution
RGBA16F color remains unchanged in size; all three histories now total
175.781250 MiB with five instances. Alignment and resize overlap are excluded.
Resources exist only while SSR is enabled.

The temporal shader uses two additional metadata reads, below the approved nine,
and retains four history taps and three output writes. Moving receiver shading
to composite removes temporal material/LUT reads. Graph bindings 17/18 now supply
hit metadata and prior transforms; temporal motion, validity and former material
reads are removed. Shared `VkrSsrParams` remains 288 bytes; a temporal-only 128-byte
camera record contains inverse current view and previous view. Native roots and
compiled layout checks remain owned by [ADR-044](044-shader-cross-backend-contract.md).
`ssr_reflection` capture version 5 denotes full-resolution incoming radiance;
version 4 was full-resolution shaded RGB. Raw capture remains version 2.

The accepted 128-frame SSR settling period and following 128-sample scene-static
accumulation remain under [ADR-037](037-portable-same-resolution-temporal-antialiasing.md).
Portable TAA caps ordinary history retention at 90% during settling. FSR and the
separate post-MetalFX accumulator retain their policies; reflected-hit rejection
must work before those final reconstruction stages.

## Consequences

Reflected-object correspondence addresses the wrong-surface trail that retention
caps alone cannot align. Incoming-radiance history lets the current receiver own
material response even when a reflected feature moves across that surface.
Unsupported coverage returns to probes immediately, which can expose trace misses
or fresh-sample noise. Screen-space visibility, finite ray budget, half-resolution
incoming detail and tangent-plane approximation still limit the result. This does
not guarantee perfect reflections or replace scene geometry and probes.

The wider history consumes the approved memory and bandwidth. Models and camera
metadata borrow existing completion-safe ownership; no per-pixel CPU allocation
or additional GPU synchronization is introduced. The existing frame control and
capture-summary compatibility remain unchanged.

## Alternatives considered

Receiver-motion reprojection follows the physical receiver rather than the
reflected feature; shorter history merely shortens its misplaced trail. Retaining
shaded history would require the previous receiver's shading weight and safe
unshade/reshade transport. Incoming-radiance history keeps material ownership at
the current pixel without another history image. A second full-resolution
composite target is unnecessary because each invocation reads/writes its own HDR
pixel. Off-screen geometry and exact curved-surface transport require capabilities
outside this bounded screen-space design.

## Evidence and remaining checks

The radiance-owner correction uses the user's later bar camera at
1784×1093 output, 80% spatial scaling, TAA enabled and SSGI disabled. Across
checkpoints 0/1/4/8, mean coverage-weighted peak-RGB range falls 26.4% on the bar
lip, while mean light rises 15.0%. The previously worst lamp pixel's TAA-output
range falls 93.5%; the whole bar-lip TAA range falls 17.6%. Final display-code range falls only
2.7%. These sampled-phase
measurements do not establish flicker-free motion: thin edges and absent current
hits still shimmer. The unchanged rejection checks pass the moving-emitter
disappearance case. Release app/editor builds, 290 shared-math outputs, temporal
SPIR-V validation, a MetalFX/SSGI camera move and serial Metal API resize pass.
[The radiance-owner record](../../assets/verification/renderer-features/ssr-radiance-owner.txt)
owns commands, payload measurements, cost limits and the remaining coverage.

The original reflected-hit implementation has the following evidence:

Release app and editor builds pass. The shared production Slang check passes
268 independently expected outputs within 1e-6, including the exact planar
previous UV 19/30 where receiver motion gives 0.65, a 21.33-pixel error at width
1280. It also covers jitter, affine and mirrored transforms, moving receivers
and hits, orthographic projection, depth/normal/identity rejection and current
receiver shading. Eleven SSR/SSGI/deferred SPIR-V modules validate with scalar
block layout; compiled reflection confirms the 512-byte Vulkan temporal root,
128-byte camera record and 80-byte retained transforms. Native Metal startup
reflection and one serial API-validated SSR/SSGI resize pass.

At the user's Bistro under-bar camera, an 18-frame in/out movement followed by
a hold produces less residual reflection error. With TAA and SSGI disabled,
raw SSR is byte-identical before/after at all five checkpoints. At the first
held frame, pre-TAA HDR error against each run's frame 48 falls 64.5% on the
countertop and 51.1% on the bar front. Frame 48 is a settling reference, not
ground truth; the bar-front frame-32 error rises 6.1%. With TAA enabled, comparison
to each revision's separate 320-warm-frame reference reduces countertop error
27.2% at the first held frame and 12.7% at frame 48; bar-front reductions are
6.2% and 3.5%. These are regional convergence measurements, not exact ghost masks.

The backend-neutral moving-emitter fixture exercises a stationary receiver with
a moving reflected object and TAA disabled. Its red reflection shifts in the
expected direction, then leaves zero red history pixels in the checked floor
region when current support disappears. Other reflections remain covered.
Layered-material and odd-size scaled editor captures have finite incoming
radiance, nonzero reflections and coverage within [0,1]. The separate SSGI
flicker camera's mean display-code range changes only 0.62% over four unsettled
checkpoints; this does not establish flicker-free output.

Matched local Release cost observations use Metal on Apple M1 Pro, source
1025×577, output 1280×720, MetalFX at 80%, three repetitions of 120 measured
frames after 60 warmup frames. Configuration, fingerprints and deterministic
work counts match; warmup is stable. Total SSR GPU mean changes
2.6968→2.8473 ms, including temporal 1.3708→1.5561 ms. These dirty/local runs
are non-authoritative and measure an intentional output/cost tradeoff.
Graph-image payload rises 52,032,592 bytes at that realized extent, consistent
with two raw-image and four history instances. GPU live allocation rises 64 MiB;
there are no measured-frame allocation creations. Full three-slot/five-history
capacity remains governed by the approved bound above.

[The reflected-hit record](../../assets/verification/renderer-features/ssr-reflected-hit.txt)
retains exact commands, report digests, measurements, compiled ABI evidence and
a screenshot. Native Windows/Vulkan execution and bilateral image comparison
remain unavailable; the shader domain stays UNALIGNED in ADR-044. Portable
snapshot payloads remain local pending authorized baseline publication.

Earlier evidence describes superseded correspondence and capture versions.
The retained [TAA settling cap](../../assets/verification/renderer-features/ssr-taa-settling-cap.txt),
[SSGI/SSR history correction](../../assets/verification/renderer-features/ssgi-ssr-history-correction.txt),
[full-resolution history](../../assets/verification/renderer-features/ssr-full-resolution-history.txt),
[shaded history](../../assets/verification/renderer-features/ssr-shaded-history.txt),
[sampling and coat occlusion](../../assets/verification/renderer-features/ssr-sampling-and-coat-occlusion.txt),
[history settling](../../assets/verification/renderer-features/ssr-history-settling.txt),
[rough history](../../assets/verification/renderer-features/ssr-rough-history.txt),
[surface traversal](../../assets/verification/renderer-features/ssr-surface-traversal.txt),
[temporal history](../../assets/verification/renderer-features/ssr-temporal-history.txt)
and [reprojection](../../assets/verification/renderer-features/ssr-reprojection.txt)
records preserve their results and limits. Version-4 shaded reflection payloads
cannot numerically validate version-5 incoming-radiance history. GPU shader
validation previously failed inside MetalTools with SSR both on and off and
supplied no shader result; it was not rerun for this change.

## Revisit when

Revisit traversal resolution, step count or filtering only with matched output
and frame-cost evidence. Selective planar reflections remain separate work for
surfaces that need reliable off-screen geometry.

## Code evidence

- [Portable controls and parameters](../../renderer/src/vkr_ssr.h)
- [Shared traversal and filtering](../../renderer/src/shaders/shared/ssr_kernel.slangh)
- [Authored graph](../../assets/render_graphs/main.rendergraph.json)
- [Frame controls](../../renderer/src/vkr_frame_input.h)
