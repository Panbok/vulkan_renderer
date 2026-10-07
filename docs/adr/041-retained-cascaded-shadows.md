---
status: implemented
updated: 2026-10-07
authority: adr
---

# ADR-041: Stable fits and retained directional shadow cascades

## Status

Accepted. Both pipeline classes render and retain the cascades. The receiver
details below are the desktop pipeline's; the tiled forward shader samples
every cascade with depth PCF/PCSS
([ADR-087](087-gpu-class-graphics-pipelines.md), decision 8).

## Context

Directional shadow quality depends on fit stability and receiver sampling.
Rerendering valid static cascades wastes work, but omission requires both a
geometric containment proof and valid retained image contents.

## Decision

Use cascaded directional depth maps with four cascades of 2048² by default.
Every preset uses that size (`VKR_SHADOW_MAP_SIZE_DEFAULT`); the harness keeps
1024 and 4096 as experiment sizes. CPU fitting
owns light-space orientation/anchor, texel snapping, fit hysteresis and optional
scene-bounds Z fitting clipped against each final cascade XY rectangle.

Track static/dynamic caster and publication generations. Pack static candidate
and instance rows per completion-protected slot; refresh on publication changes
and copy dynamic ranges independently. Retained depth follows the caster
publication generation instead
(`VkrWorldPassPayload.caster_publication_generation`): the completions of
material, texture and sampler publications a shadow reads
([ADR-019](019-bounded-forward-spatial-lighting.md)), not geometry uploads,
whose meshes join the caster set through the static generation. A static change whose listed box misses a
retained cascade's fit volume, tested as a dynamic caster's sphere is, leaves
that cascade valid and moves it to the new generation. Each physical target-image cascade keeps
its submitted fit and signature. A guard-contained static cascade with valid
retained content can omit its authored pass only when its actual submitted fit
matches the common desired projection. The newest compatible submitted static
fit containing the current cascade supplies that projection. Other physical
images render into their own depth storage using the same descriptor when next
acquired; selecting its CPU metadata never borrows another image's depth.
Dynamic overlap, generation drift,
invalid contents or incomplete publication forces rendering. Scene-authored
meshes and editor shapes are static casters, because their transforms and
geometry change only through mesh-manager calls that bump the static
generation, adding and removing a drawn mesh included; an edit therefore
redraws once. Runtime-created instances and
skinned meshes stay dynamic, and a dynamic caster that overlaps a cascade or
local light redraws it every frame. Pending fits and
content validity commit only after successful submit. Reused cascades publish
the fit that actually produced their depth.

A retained cascade keeps its culling view and classification, so
`draw.shadow.cascadeN.indirect_commands` still counts the casters behind its
depth, but encoding writes no commands for it: Metal sets the view's
`encode_idle` and skips its indirect-command reset, and Vulkan sets its bit in
`VkrVulkanCullRoot.encode_idle_view_mask`. The view is idle exactly when
`shadow_cascade_render_mask` leaves its graph pass uninstantiated, so a stale
command range is never executed and the next redraw resets it first. Local faces achieve the same by omitting reused
faces' views (ADR-019).

Classification skips a caster in a directional cascade when its world
bounding sphere spans less than one cascade texel
(`vkr_gpu_cascade_caster_too_small`, `VKR_GPU_CASCADE_CASTER_MIN_TEXELS`). The
test reads the cascade's LOD view, whose constant scale is texels per metre;
it applies to every non-camera orthographic view, so local faces and an
orthographic editor camera keep every caster. The texel size follows the
submitted fit, so the skipped set is fixed for a retained cascade.

Each cascade keeps the light direction its fit was framed with while a moving
light stays within that cascade's tolerance: 0.025 degrees for cascade 0,
doubling for each farther cascade, whose texels are larger. The fit, light view
and light signature follow that direction, so a retained cascade stays reusable
and its receivers sample the matrix that produced its depth. A cascade adopts a
light that turns past its tolerance at once. A light whose direction stays
unchanged for four consecutive updates has stopped and is adopted exactly by
every cascade, so a stopped or static sun never keeps an approximate shadow;
fewer repeats may be a simulation that turns the sun only every second to
fourth rendered frame. The tolerance applies
only with fit stabilization; zero adopts every change. Direct lighting still
uses the exact direction.

The optional proactive refresh scheduler selects low-margin cascades within a
bounded budget; the production budget is zero. Converging stale image copies is
a correctness refresh independent of that optional budget. ADR-033's SDSM is also opt-in.
GPU camera/cascade classification remains ADR-028's one-phase topology.

Receivers use shared progressive rotated Poisson comparison-PCF at 1/4/9/16/32
taps, an optional uniform-region early-out at high tap counts, cascade cross-fade
and distance fade. Constant/slope/normal-offset bias is authored in shadow texels
and converted using each cascade's texel size and fitted depth span. Backend
raster-bias lowering preserves those units. Cutout casters use alpha testing.

The nearest two cascades use contact-hardening PCSS when the authored sun angular
diameter is positive. Eight progressive nearest-depth samples, starting at the
center, estimate the average blocker depth. Their search radius is the larger of
the existing PCF radius and the receiver's distance from the fitted light near
plane multiplied by `tan(diameter / 2)`. The filter radius uses the average
blocker-to-receiver distance with the same tangent. Both distances convert from
normalized depth through the fitted depth span, then into shadow texels.

PCSS filtering uses the configured tap count capped at sixteen, including the
no-blocker fallback. An empty sparse search retains the existing PCF radius;
it does not prove the receiver is lit. Farther cascades and zero angular diameter
retain the existing PCF path and tap count. Existing bias, kernel rotation,
uniform-region early-out, cascade cross-fade and distance fade remain active.

Scene directional lights author `sun_angular_diameter_degrees`, defaulting to
0.53 degrees, with finite values in `[0, 5]`, the sky disc's limit, because the
same diameter sizes the visible disc (ADR-058). Loading clamps older values up
to 180 degrees to that limit. Scene loading, editor changes and
undo persistence retain this value. It controls the shadow-filter approximation;
it does not turn directional BRDF evaluation or offline baking into disk-light
transport. Frame input version 36 stores its half-angle tangent in the fourth
component of `origin_inv_size_sun`; the cascade record remains 96 bytes. The
runtime converts degrees once per frame. Changing this value changes receiver
sampling and temporal radiance validity without invalidating retained depth maps.


### Far-cascade EVSM

The **Filtered far shadows** setting (`VkrShadowConfig.far_cascade_evsm`) gives
cascades from `VKR_SHADOW_EVSM_FIRST_CASCADE` (2) exponential variance moments.
The shadow payload's `evsm_enabled` requires more cascades than that. The High
and Ultra shadow presets and the High and Epic graphics presets turn it on; the
Balanced shadow preset and the Low and Medium graphics presets leave it off.
Harness children follow their case's shadow preset unless `VKR_SHADOW_EVSM` is
`1` or `0` ([ADR-051](051-renderer-harness-and-evidence.md)). The graph's
`shadow_moments` image is `R32G32B32A32_SFLOAT` at half the depth map's side,
one layer per filtered cascade, with no mip chain, retained per target image
like `shadow_map`. `Shadow.Moments.${i}` runs only for a filtered cascade that
redraws (`shadow_moments_render_mask`). Each moments texel takes the 4x4 depth
texels around its 2x2 footprint with (1, 3, 3, 1) tent weights per axis and
stores the weighted moments of the warps `exp(40 d)` and `-exp(-5 d)`, with `d`
the normalized depth remapped to `[-1, 1]`. Desktop `Lighting.Deferred` samples
a filtered cascade with one bilinear fetch and takes the smaller of the two
one-sided Chebyshev bounds against the PCF reference depth. The minimum
variance follows each warp's slope, and a 0.2 light-bleeding reduction clips
the bound. Cascades 0 and 1, desktop forward, transmission and froxel shading,
and tiled forward shading keep depth PCF/PCSS; the cascade cross-fade blends
PCF and EVSM visibility. The tiled pipeline renders no moments: its graph has
no moments pass, Metal implements none, Preferences greys the setting, and the
runtime clears `far_cascade_evsm` on tiled frames so far cascades stay
reusable without moments.

Moments derive from retained depth, so `VkrRetainedShadowToken` reports
`moments_valid_cascade_mask` from the moments image's per-layer content
validity, and a filtered cascade is reusable only with valid moments as well.
Changing the setting or, with it on, the cascade count also discards the fit
history so every target image redraws once. Off declares no image or pass and
keeps the receivers' PCF path exactly.

## Consequences

Fit and content retention reduce repeated work only when every reuse condition
holds. Independent per-image projections can remain different after camera
movement, alternating shadow sampling and preventing checked temporal scene
signatures from matching. Converging them can add up to one render per stale
image/cascade for each adopted fit, spread across normal completion-safe image
reuse. Once the images agree, static frames omit those passes again. No new GPU
storage, copies or waits are required.

While the light moves, a cascade's shadow can lag the lit direction by up to
its tolerance: under 7 mm for a 15 m caster in cascade 0 and 5.2 cm in cascade
3 of four. At a 24-minute day, about 0.004 degrees per 60 Hz frame, cascade 0
refreshes about every sixth frame and cascade 3 about every fiftieth instead of
all four every frame, and each refresh also renders the other target images as
they are reused. A light that turns past a cascade's tolerance every frame
redraws it every frame, as before. More aggressive fitting/bias/filter choices alter quality and must be
measured. Point/spot shadows use ADR-019's independent bounded pool; arbitrary indirect-light occlusion remains absent.

## Alternatives considered

Rendering every cascade is the safe forced-update control. Retaining allocation
without content validity is insufficient. Two-phase visibility was declined in
ADR-032; SDSM is not the default quality policy. SDSM moves resolution but not
raster or sampling work, and each fit change invalidates a retained cascade,
so it does not lower current shadow cost; the Vulkan Bistro study below
confirms this.

4096² cascades were rejected: in Bistro they cost 0.4 ms more cascade work per
orbit frame and 905 MiB more GPU memory than 2048², for differences that are
hard to see at 2x zoom. 1024² cascades were rejected because they visibly blur
thin near shadows. See [Cascade map size evidence](#cascade-map-size-evidence).

Full-resolution EVSM for every cascade was rejected: with mips, `RGBA32F` at
2048² costs 85 MiB per cascade per target image, beyond the 16 GB Mac floor
(ADR-083), and the near cascades already harden at contact through depth
PCSS. EVSM for the local atlas was rejected too: an `RG32F` layer is four times
the D16 layer, faces of 128 to 1024 texels need per-face blurs with gutters,
hardware filtering across cube-face borders breaks the per-tap face remap, and
one moments fetch cannot keep the per-tap combination of opaque depth and
receiver-gated transmission (ADR-019).

## Contact-hardening evidence

Production Release and editor wrappers pass. A compiled shared Slang helper
checks the nearest-two-cascade gate and world/depth/texel conversion. Captures
on the Metal desktop implementation, removed on 2026-10-06, used equal plates
at 0.1 m and 8 m above a receiver, with 0, 0.53 and
2-degree sun sizes. With a 4096-square shadow map and 768-square output, the
far shadow's 10–90% transition covered 400, 479 and 818 pixels respectively.
The near-contact region and every pixel outside the far-shadow region remained
byte-identical; all three raw depth maps were identical. These are output
checks, not a performance comparison. Neither native Vulkan nor the tiled
pipeline has run them.

## Moving-light evidence

Bistro with the atmosphere, on the Metal desktop implementation, removed on
2026-10-06 (Release, M1 Pro), 1280×720 offscreen, 60 warmup and 600 measured
frames, with a temporary diagnostic turning the sun 0.0042 degrees per frame
from the phase start. Single-process, non-authoritative runs. With the
tolerance, whole-submission GPU time is 25.90 ms at p50 and 35.80 ms at p95;
the same binary with the tolerance forced to zero gives 33.49 and 43.43 ms.
Cascades 0-3 render in 200, 100, 50 and 24 of the 600 frames. At 0.6 degrees
per frame every step exceeds every tolerance and GPU time is unchanged, 33.26
ms with the sun up and 24.44 ms with it set. A CPU test turns a retained light
by 0.01, 0.03 and 5 degrees and stops it: nothing, cascade 0, and every cascade
render, and a light repeated four times is adopted exactly. The static Bistro
snapshot is unchanged.

## Far-cascade EVSM evidence

On the Metal desktop implementation, removed on 2026-10-06 (Release, M1 Pro,
2026-10-05, dirty tree, non-authoritative). With the setting off, the Bistro
street capture was byte-identical to the capture before EVSM existed.
`shadow_bistro_far_cascade_capture` (long street view, 1280x720, no TAA; now a
Vulkan case) with `VKR_SHADOW_EVSM=1` changed 0.36% of pixels by more than 2 of
255 (maximum 50), along distant facades in cascades 2 and 3, with no visible
acne or bleeding. `shadow_bistro_far_cascade_perf`, a Metal desktop case since
removed, under `local-offscreen-perf-audit-gpu` (five children of 300 frames)
measured `Lighting.Deferred` at 3.65 ms against 3.68 ms p50 (spread 0.035 ms)
and an unchanged frame. With a temporary diagnostic forcing every cascade to
redraw, `Shadow.Moments.0` and `.1` each took 0.25 ms per redraw against
1.8–2.1 ms per cascade raster, which was unchanged. The moments add 32 MiB per
target image at the High preset's 2048² maps. The Vulkan shader-ABI reflection
test checks both roots against the compiled SPIR-V.

On native Vulkan (Release, RX 6700 XT, 2026-10-07, dirty tree, one binary for
both arms, non-authoritative), with Bistro's authored sun at 35 degrees
elevation, `VKR_SHADOW_EVSM=1` against the setting off gave these results:

- `shadow_bistro_far_cascade_capture` under `local-offscreen` changed 0.33% of
  pixels by more than 2 of 255 (maximum 51), all on distant facades in cascades
  2 and 3. Of those pixels, 95% became brighter, at the edges of thin facade
  details. The capture shows no acne or visible light bleeding.
- In the street view of `bistro_shadow_quality_lambda080`, where cascades 2 and
  3 cover only the plaza and tree seen through an arch, final color changed by
  at most 1 of 255. The `shadow_debug_*` channels cannot show the difference:
  they filter every cascade with PCF.
- `shadow_bistro_far_cascade_perf` under `local-offscreen-perf-audit-gpu` (two
  children of 300 frames, static camera, so no cascade or moments redraw)
  measured `Lighting.Deferred` at 1.767 ms against 1.776 ms p50 (standard
  deviation 0.033 ms).
- `bistro_shadow_orbit` under `local-windowed-gpu` (two children of 300 frames)
  measured `Shadow.Moments.0` at 0.078 ms and `.1` at 0.072 ms p50 per redraw,
  in 42 and 60 of 600 frames. Cascade raster time and draw counts did not
  change, and `Lighting.Deferred` measured 1.343 ms against 1.345 ms p50. Live
  GPU memory rose by 96 MiB, 32 MiB for each of the case's three target images.

These results made far-cascade EVSM the desktop default on 2026-10-07. In
Bistro the setting costs no measurable frame time, and its visible effect is
small.

## Cascade map size evidence

On native Vulkan (Release, RX 6700 XT, 2026-10-07, dirty tree, one binary for
every run, non-authoritative), far-cascade EVSM on, Bistro's authored sun at
35 degrees elevation, the cascade side was set through the case's
`renderer.shadow_map_size` to 1024, 2048 and 4096, each with SDSM off and on
(`renderer.shadow_sdsm`). Captures ran under `local-offscreen`, the timing of
`shadow_bistro_far_cascade_perf` under `local-offscreen-perf-audit-gpu`, and the
timing of `bistro_shadow_orbit` under `local-windowed-gpu`. Each timing run had
two children of 300 frames.

Quality, as final-color pixels that differ by more than 2 of 255 from 4096²
with the same fitting:

| View | 1024² | 2048² |
|---|---|---|
| `shadow_bistro_plaza_near_capture`: sunlit paving in cascade 0 | 3.65% (maximum 67) | 1.18% (maximum 46) |
| `shadow_bistro_far_cascade_capture`: distant facades | 1.12% (maximum 49) | 0.44% (maximum 38) |
| Street view of `bistro_shadow_quality_lambda080`, near street in shadow | at most 2 of 255 | at most 1 of 255 |

In the plaza, 1024² widens and softens the bollard shadows and loses the
shadows of facade relief. 2048² and 4096² are hard to tell apart at 2x zoom.

Cost in the orbit, where cascades redraw as the camera moves:

| Side | Redraws of cascades 0/1/2/3 in 600 frames | Cascade and moments GPU time per frame | `Lighting.Deferred` p50 | Frame p50 / p95 | Live GPU memory |
|---|---|---|---|---|---|
| 1024 | 72 / 0 / 12 / 24 | 0.041 ms | 1.310 ms | 4.51 / 6.72 ms | 5290 MiB |
| 2048 | 146 / 0 / 42 / 60 | 0.125 ms | 1.335 ms | 4.72 / 6.93 ms | 5453 MiB |
| 4096 | 270 / 12 / 84 / 126 | 0.532 ms | 1.439 ms | 5.23 / 7.57 ms | 6358 MiB |

The reuse guard band is a texel count, so it covers half the distance at each
doubling of the side. A larger map therefore redraws more often as well as
costing more per redraw. With a static camera no cascade redraws, and
`Lighting.Deferred` in the far street view took 1.716, 1.753 and 1.852 ms p50.
The memory includes the case's three target images.

SDSM did not pay for itself (ADR-033). At 2048² in the orbit it raised the
redraws to 196 / 66 / 36 / 54 and the cascade, moments and `SDSM.Reduce` time
to 0.190 ms per frame, and the frame p95 to 7.25 ms. At 4096² the frame p95
rose to 9.26 ms. `SDSM.Reduce` took 0.027 ms every frame. Against fixed splits
it changed 1.07% of plaza pixels and 1.14% of far-street pixels by more than 2
of 255 with no visible gain in the crops. The SDSM orbit at 1024² redrew
different cascades in its two children, so the harness marked it incomplete
for a work-volume mismatch.

Report digests (prefixes): orbit at 1024, 2048 and 4096 with fixed splits
`sha256:3e12b5c79922`, `sha256:425eb1e72c5a` and `sha256:8476cf0963a8`; with
SDSM `sha256:68d09855617a` (incomplete), `sha256:a830cfddfe18` and
`sha256:5d37452a303f`. Plaza captures with fixed splits
`sha256:70ce061372e0`, `sha256:1e7a37f46cb8` and `sha256:7f359d236056`.

## Retained-cascade culling evidence

On the Metal desktop implementation, removed on 2026-10-06 (Release, M1 Pro,
2026-10-05, dirty tree, non-authoritative), `bistro_native_perf_audit_steady`,
a Metal desktop case since removed, under `local-offscreen-perf-audit-gpu`
(five children of 300 frames, every cascade retained) measured `Cull.Encode` at
0.035 ms against 0.159 ms p50, with `Cull.Classify` unchanged at 0.023 ms.
An earlier variant that also rejected every candidate in classification
saved a further 0.008 ms but zeroed the retained cascades' command counts that
twelve cases assert on. A temporary diagnostic that redraws one cascade per
three frames in rotation, leaving the other views idle, produced cascade depth
captures byte-identical to a normal run's for all four cascades. Native Vulkan
execution is not yet recorded.

## Sub-texel caster evidence

On the Metal desktop implementation, removed on 2026-10-06 (Release, M1 Pro,
2026-10-05, dirty tree, non-authoritative), Bistro long street view. With a
temporary diagnostic redrawing every cascade each frame,
`shadow_bistro_far_cascade_perf` under `local-offscreen-perf-audit-gpu`
measured `draw.shadow.cascade{1,2,3}.indirect_commands` at 2,740, 2,548 and
1,140 against 2,862, 2,909 and 2,909, and `Shadow.Cascade.3` at 1.506 ms
against 1.758 ms p50 per redraw; cascades 0 to 2 changed by at most 0.024 ms.
The before run's report is incomplete only for local-shadow work-volume
variation between repetitions. Captured cascade 2 depth is byte-identical and
cascade 3 differs in one of 4.2 million texels, by 2.5e-4, so the skipped
casters drew no visible depth; final color differs only by run-to-run noise.
Native Vulkan execution is not yet recorded.

## Revisit when

A focused scene exposes containment, bias, transition or distance artifacts, or
matched quality/cost evidence justifies changing defaults. The two Bistro views
that support the far-cascade EVSM default put few sunlit receivers behind
overlapping casters in cascades 2 and 3, so EVSM light bleeding is mostly
untested. Turn the default off, or compare four-moment shadow maps, when a
scene shows that bleeding.

## Implementation

[`vkr_shadow_system.c`](../../runtime/src/renderer/systems/vkr_shadow_system.c),
[`vkr_candidate_residency.h`](../../renderer/src/vkr_candidate_residency.h),
[`shadow_kernel.slangh`](../../renderer/src/shaders/shared/shadow_kernel.slangh), and
[`main.rendergraph.json`](../../assets/render_graphs/main.rendergraph.json).
