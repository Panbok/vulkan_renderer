---
status: implemented
updated: 2026-10-12
authority: adr
---

# ADR-055: Opaque screen-space reflections

## Status

Accepted. SSR belongs to the desktop pipeline, and Vulkan is its only
implementation ([ADR-087](087-gpu-class-graphics-pipelines.md)).
Full-source-resolution tracing, reflected-hit reprojection and
incoming-radiance history are implemented. On 2026-10-11 GGX lobe sampling
with ray reuse, the receiver-plane correspondence, the exhaustion state, the
cubic history fetch and the history rule below replaced the mirror-ray
filter and its revision-based invalidation. On 2026-10-12 the no-hit hold
with rough zero samples, the receiver-plane tolerance, the 12-tap cubic
history and its bounds, bilinear history for rough receivers, the hit
directions, the signature's point-light rule and the exact-predecessor
history without final TAA took their current form below. Native Vulkan
evidence for them was recorded on 2026-10-12 on one Windows host from a
dirty tree and is not authoritative
([evidence](#evidence-and-remaining-checks)); the older output evidence
comes from the Metal desktop implementation, removed on 2026-10-06.

## Context

Saved scene probes supply stable specular lighting outside the current view.
Opaque screen-space reflections can supply visible local geometry and motion,
while retaining those probes where the screen cannot establish a hit.

The existing culling HZB is prior-frame history and is skipped during raster
jitter. GTAO's depth chain is conditional and does not establish the conservative
hierarchy needed for reflection traversal.

## Decision

SSR is optional on opaque surfaces with perceptual roughness at most 0.6.
Each covered source pixel owns its ray. Tracing uses an independent current-frame
positive-depth pyramid, full-resolution leaves and at most 48 hierarchy decisions.
The pyramid begins at floor-half extent; its odd-extent reductions retain the final
rows and columns independently of trace resolution. Zero depth means uncovered.
Coated pixels trace the coat; uncoated pixels trace the base. Misses retain probes.

The default `high` tracing preset preserves these settings. The opt-in
`balanced` preset limits traversal to 24 hierarchy decisions and excludes
receivers rougher than 0.35. Select it before renderer creation with
`VKR_SSR_QUALITY=balanced`; `high` or an unset variable retains the reference.
Unknown values fail initialization. Harness reports record the effective tier
and workload fingerprints distinguish balanced from high when SSR is enabled.
Use the same environment selection for every invocation in a comparison.
Native API callers may initialize their cold controls through `vkr_ssr_config_for_quality()` and tighten them further.
Quality selection requires renderer recreation, so a tier cannot reuse history
produced with another tier. The balanced tier retains full-resolution
resources, lobe sampling and ray reuse, current-hit
support, reflected-object identity validation and all history rejection rules.
Excluded receivers and traversal misses use the existing probe fallback.
Its reduced work bounds do not establish a measured speedup or reduced image
memory: matched capture-free Release timing and Bistro output checks remain
required on native Vulkan. The `ssr_tier_bistro` snapshot and
`ssr_tier_bistro_cost` profile cases expose the same 640×360 Bistro workload
under each environment selection; their prepared cases are not measured evidence.
The existing harness fingerprint test independently requires unset/explicit high
identity and a distinct balanced workload fingerprint.


Traversal uses homogeneous interpolation, clipped screen bounds, absolute cell
crossings and the earliest supported rear-depth slab. A cell exit restarts the
ray `1e-6` past its crossing parameter. FP32 can store a cell boundary `k/N`
below pixel edge `k`, so that restart can select the exited cell again. The
restart then moves 1/64 source pixel along the projected segment's major axis
(`VKR_SSR_RESTART_PIXELS`). SSGI shares this restart. Thickness is a binary
intersection tolerance; a separate 1 mm normal offset separates the origin.
Leaf validation reuses its loaded depth and accepts only its half-open source
pixel, allowing numerical roundoff ahead of the surface. A hit samples
incoming HDR once, bilinearly, at its fractional coordinates. SSGI shares the
traversal with its own start level and hit slab
([ADR-060](060-screen-space-diffuse-indirect-lighting.md)). Camera rays
unproject device depths 0.75 and 0.5 (`VKR_SSR_RAY_DEVICE_DEPTH_A/B`), both
on the near side under forward and reversed depth
([ADR-109](109-reversed-camera-depth.md)), never the far plane, where float32
loses the ray's direction.

The user approved GGX lobe sampling, ray reuse and the history changes below
on 2026-10-11. A receiver whose packed roughness is at most 0.04
(`VKR_SSR_MIRROR_ROUGHNESS`) traces its exact mirror ray. A rougher receiver
traces one direction sampled from the GGX distribution of visible normals
(Heitz 2018) of its selected lobe: the coat when active, which is isotropic
([ADR-062](062-layered-clearcoat.md)), otherwise the base with its authored
anisotropy along the decoded axis ([ADR-064](064-anisotropic-ggx-reflection.md)).
The lobe takes the normal-filtered roughness that the composite and the
environment lookup use. The 256-phase Halton(2, 3) sequence with a per-pixel
Cranley-Patterson rotation that SSGI uses (`vkr_ssgi_sample_2d`), indexed by
the frame's `sample_phase`, selects the sample; its period stays a multiple of
TAA's 8 and GTAO's 64 phases. Harness replays start warmup at this
sequence's zero whenever the last submitted frame ran SSR or SSGI
([ADR-051](051-renderer-harness-and-evidence.md)), so a replay's rays
depend only on its case frame. A sample below the surface traces nothing.
Each pixel still traces one ray.

Trace writes full-source-resolution RGBA16F incoming radiance and an
RGBA32_UINT ray record. A hit record holds its UV as two 16-bit unorms, its
positive view depth, the ray's pdf and the visible-draw row that identifies
the reflected instance. A miss record holds the ray's octahedral direction as
two 16-bit snorms, its state and its pdf: MISS when the ray left the screen
or its distance, UNKNOWN when it ran out of hierarchy decisions. An all-zero
record means the receiver traced no lobe ray. Raw alpha is the edge and
depth confidence of the one sample. Non-finite radiance stores as zero and
finite radiance saturates at 65504 (`vkr_ssr_storable`) in the trace, in
every gathered sample and history tap, and in the history output.

Temporal runs at full source resolution. A rough receiver resolves its own
ray and eight neighbouring rays on a Vogel spiral of radius 1 to 3 source
pixels, which grows with roughness across [0.04, cutoff] and rotates per
pixel and per frame; every reused ray lies at least one pixel from the
centre. A mirror uses its own ray only. A reused ray's weight is the
receiver-plane weight times the spatial weight times the receiver's GGX
value toward the ray's hit, `D G2 / (4 N·V)` without Fresnel, over the
ray's pdf, capped at 4 (`vkr_ssr_reuse_weight`). Directions to hits start
at the trace's ray origin, 1 mm along the lobe normal
(`VKR_SSR_RAY_ORIGIN_BIAS`), so the own ray is evaluated along the direction
it was traced in and its ratio is G2/G1. The receiver-plane weight measures
a neighbour's distance from the centre receiver's tangent plane against the
larger of 2 cm and a quarter of the separation that plane predicts at the
neighbour's pixel: the distance from the centre to where the neighbour's
camera ray meets the plane, capped by the measured separation
(`vkr_ssr_plane_separation`). A neighbour on the plane, such as a far texel
of a grazing floor, keeps its measured separation, so grazing floors and
water reuse rays; a neighbour on another surface cannot widen its own
tolerance. A normal term reaches zero at 60 degrees
(`vkr_ssr_receiver_plane_weight`). The result estimates the lobe's
normalized incoming radiance, the quantity the split-sum prefilter
approximates, and the composite applies one receiver weight to it and to
the environment. Coverage is the hit share of the known weight; rays of
unknown outcome count in neither. The same samples supply the clamp
bounds. Temporal history stores incoming light, so history lookup does not
transport the previous receiver's material, BRDF or GTAO response.

The user approved reflected-hit reprojection to replace receiver-motion lookup.
Only rays of the output pixel's own draw can supply the reflected hit; rays
of another surface still add current radiance when they pass the plane test.
Among them the gather keeps the ray with the largest component of weighted
RGB entering the numerator, without averaging hit positions across objects.
Covered weight breaks equal-energy ties; traversal order resolves exact
ties. Ranking coverage alone can tag a lamp-dominated mixture with a dark
object's geometry, rejecting otherwise reusable lamp history as raster
jitter moves the samples. History geometry and identity are written for
every tracked receiver; virtual depth is zero when the pixel has no hit of
its own.

Current and retained instance models transport both the receiver plane and hit
into the selected producer's camera. Point transport supports nonsingular affine
models; normal transport uses the corresponding inverse transpose. Prior transform
validity, generation and frame must match for both instances. The selected
transform buffer also owns that producer's camera view matrix, published only
after successful submission. History works with TAA disabled.

For plane point `P`, unit normal `n` and hit `H`, the virtual reflected point is
`Q = H - 2*n*dot(n,H-P)`. Reprojection follows `Q_t = P + (Q - P) * parallax`:
parallax is 1 for mirrors and below roughness 0.15 and falls by smoothstep to
0 at 0.4 (`vkr_ssr_reflection_parallax`), since a wide lobe blurs the image
over more than its parallax. Projecting the current and transported prior points
gives a UV motion delta. Adding this delta to the current full-resolution
pixel preserves its offset from a neighboring traced receiver. Reusing the
current jittered projection requires the selected producer's previous-minus-current
jitter, retained at parameter offsets 280/284. The prior UV ray intersects the
transported receiver plane to determine expected receiver depth; receiver-motion
depth describes a different point and is not used.

Each history tap compares its stored receiver depth with the depth of the
transported receiver plane along that tap centre's camera ray
(`vkr_ssr_plane_depth_prepare`, `vkr_ssr_plane_depth_at`), and its stored
selected normal with the transported one (dot product at least 0.9). The
reflected-hit candidate also compares virtual depth. Instance identity is
checked only where transport is not the identity: the receiver instance
when the receiver moved, the reflected instance when the receiver or the
reflected instance moved (`VKR_SSR_MATCH_*`); static receivers and reflected
objects need matching geometry only. A receiver whose lobe roughness is
below 0.4 (`VKR_SSR_PARALLAX_ROUGHNESS_END`, reflection parallax above
zero), with the inner 2×2 taps accepted and at least one of them covered,
takes a coverage-weighted 12-tap Catmull-Rom: the 4×4 footprint without its
four corners, whose weights stay below 0.006 each, divided by the kept
weight sum, so a uniform history keeps its coverage. A rejected outer tap
takes its nearest inner tap's colour. Its radiance is clamped to the range
of the covered inner taps and its coverage to the inner taps' coverage
range, so ringing cannot leave the range a bilinear result could take or
brighten the rough residual or a held reflection (`vkr_ssr_history_cubic`).
A receiver at roughness 0.4 or above, whose lobe blurs the image more than
bilinear resampling does, always resolves the accepted bilinear taps
(`vkr_vk_ssr_history_fetch`); the user approved both history changes on
2026-10-12. When an inner tap is rejected, when no inner tap is covered (the
eight outer taps are then not read), or when the cubic keeps less than half
the bilinear coverage, as negative lobes at a coverage edge can, accepted
bilinear taps resolve it too. Support is the accepted bilinear weight, or
one for the cubic. A receiver first fetches the history of its
reflected-hit candidate when it has one, otherwise of its own surface
motion; below half reflected-hit support the surface motion is fetched as a
second candidate, and the candidate with more support wins. Both candidates
share one transported receiver plane. For a glossy receiver that fallback clamps
to the current hits' mean plus or minus one standard deviation, inside their
range, and keeps no residual; for a fully rough receiver (parallax 0)
surface motion is the normal rule with the normal clamp. Unsupported
transforms, degenerate or grazing intersections and rejected taps use current
radiance or probes. Curved and normal-mapped receivers remain a local
tangent-plane approximation.

History RGB retention is the motion/roughness-adjusted weight times the
accepted support. Defaults range from 0.85 during motion to 0.95 for
stationary rough receivers. Motion is the reprojected point's motion with
jitter removed. Current bounds clamp mirrors; roughness continuously relaxes
clamping, with at most half the out-of-bounds RGB residual surviving an
update. Constant sparse radiance stays constant. With current hits, history
also fills the unknown share of the estimate.

With no current hit, history is held only while the rays could still be
missing a real reflection (`vkr_ssr_temporal_filter`). A mirror's own ray is
exact: its known miss returns to the environment term at once, and only an
unknown outcome holds its history, in proportion to the unknown share times
the accepted support. A glossy lobe (reflection parallax above zero, lobe
roughness below 0.4) samples an image of the scene, so all of its rays
missing is evidence that the reflected object left. With `n` rays of known
outcome and history coverage `c`, the evidence is the accepted support
times one up to three expected hits `n c`, then `exp(3 - n c)`
(`vkr_ssr_miss_plausibility`). Each such frame multiplies the held coverage
by that evidence and by 0.5 (`VKR_SSR_HOLD_RETENTION`), and coverage at or
below 1/16 (`VKR_SSR_HOLD_COVERAGE_MIN`) returns to the environment term,
so a glossy or mirror hold ends within four frames from full coverage, and
a history of coverage 1/8 or less ends on the first. A rough lobe
(parallax 0) scatters its rays, and a sparse hit such as a small lamp
misses with every ray on some frames. For a rough lobe a frame without
hits, including rays that ran out of decisions, is one more sample of zero
coverage: coverage is multiplied by the temporal retention (0.95 for a
still rough receiver) and keeps no floor, so it converges to the lobe's
hit share (user decision, 2026-10-12). Misses that refute a covered rough
lobe multiply it by `exp(3 - n c)` and the retention and end at the floor;
nine rays at coverage 1 end it at once. Rays of unknown outcome never count
as misses: they add no expected hits. A rough receiver reads history on an
all-miss frame; a mirror whose ray is a known miss does not.

Composite loads filtered incoming radiance at its current source pixel, applies
that pixel's BRDF/GTAO once, and computes
`HDR - coverage * current_probe + coverage * current_shaded_reflection`.
It scales the history's coverage by the receiver's packed roughness: one up
to 0.15 below the cutoff (0.45 at the default 0.6), then linearly to zero at
the cutoff (`vkr_ssr_roughness_confidence`), so a valid hit fully replaces
the environment term below the fade and coverage is continuous at the
eligibility edge. A non-finite composite sample is dropped.
Base SSR shares the deferred base-normal-filtered BRDF, sheen allocation and
anisotropic response with its current probe replacement. Coat SSR uses filtered
selected-normal roughness; removed coat probe light uses packed roughness and
the coat-directed GTAO cone to match deferred lighting exactly. Base probe light
remains on coated pixels. Composite writes only its own HDR pixel after trace
finishes sampling it and before transmission builds its background pyramid.

The graph owns all images and the declared transform-history read. The selected
color/geometry/identity tuple and transform buffer must share a producer
submission, frame and scene. Reading that exact producer while in flight uses
Vulkan's same-queue write/read barriers. Every reader extends last use; output
reuse and retirement require producer and reader completion.
The scene generation, the SSR content signature (lighting without the
point-light table, sky, sun-shadow configuration, GTAO and feature toggles;
`vkr_ssr_content_signature` in
[`vkr_temporal.c`](../../renderer/src/vkr_temporal.c)), the graph revision,
temporal resets (cuts, projection, extent, scene or mode changes) and the
image size invalidate incompatible history. Texture uploads, material and
asset publications, pending or unloaded visible assets and fog do not: the
per-pixel receiver, hit, depth, normal and transform tests reject changed
geometry, and the current-sample bounds limit changed light (user approval,
2026-10-11). An authored fog change still requests the temporal scene-change
reset ([ADR-057](057-analytic-height-fog.md)). Since 2026-10-12 the
point-light table does not either: the scene table ranks point lights by
camera distance and fades a light in or out when its selection changes
([ADR-019](019-bounded-forward-spatial-lighting.md)), so it changes while
the camera moves, and the current-sample bounds limit the light a change
adds, as they do a fading shadow strength (`temporal_scene_lighting`).
Portable TAA's own signature still hashes every point light
([ADR-037](037-portable-same-resolution-temporal-antialiasing.md)). No extra
wait or history instance is introduced.

The selected tuple is the immediately preceding frame's, with or without
final TAA ([ADR-037](037-portable-same-resolution-temporal-antialiasing.md)),
so history never skips a submitted frame and its age does not follow GPU
timing. The depth history leaves its producer in `GENERAL`; its only reader,
the next frame, moves it to the sampled layout once
(`vkr_vk_prepare_ssr_history`). A harness capture copy of a graph image
returns the copied subresource to its graph layout afterwards
([`vkr_vulkan_capture.c`](../../renderer/src/vulkan/vkr_vulkan_capture.c)),
so the next frame's history reader barriers from the layout it states.
`post.screen_history.state`, a frame-domain u64 gauge, reports SSR history
in bits 0 to 7 and SSGI history in bits 8 to 15 for every submitted frame
(`VkrScreenHistoryState`). In each byte, bit 0 means history selected,
bit 1 a temporal reset, bit 2 a different content signature, scene
generation or graph revision, bit 3 a different image size, bit 4 no
complete tuple from the immediate transform predecessor, and, for SSGI only,
bit 5 a different retained shadow state. A feature that did not run leaves
its byte zero, so a frame with both histories selected reads 257. Editor
logs of 2026-10-11, before this metric, reported 511 of the 512 catalog
slots (`VKR_METRICS_MAX_SLOTS`) in use, so GPU counters of exhausted rays
or accepted history need a catalog capacity decision first.

The user approved testing full-resolution rays while preserving ghost rejection,
with at most 64 MiB additional logical images for three slots or 169 MiB for eight
at source 1280×720. The retained implementation removes the redundant receiver
coordinate image, so this change adds 42.1875 / 112.5 MiB over reflected-hit SSR.
One ray per source pixel is nominally four times the prior floor-half grid;
odd rounding gives 4.002805 times at 1427×874.

Combined with the earlier approved reflected-hit history expansion, the resource
change at source 1280×720 is:

| Resource | Format and extent | Three frame slots / five histories | Eight frame slots / ten histories |
| --- | --- | ---: | ---: |
| Raw radiance | Half → full-source RGBA16F, per frame image | +15.820313 MiB | +42.187500 MiB |
| New raw hit | Full-source RGBA32_UINT, per frame image | +42.187500 MiB | +112.500000 MiB |
| Removed receiver coordinate | Half-resolution RG32_UINT, per frame image | −5.273438 MiB | −14.062500 MiB |
| Geometry history | Full-resolution R32F → RGBA32F | +52.734375 MiB | +105.468750 MiB |
| Identity history | Full-resolution RG32_UINT → RGBA32_UINT | +35.156250 MiB | +70.312500 MiB |
| Total increase | Logical image payload | **140.625000 MiB** | **316.406250 MiB** |

RGBA32F geometry stores receiver depth, virtual depth and two signed octahedral
components of the selected view-space normal. RGBA32_UINT identity stores the
receiver and reflected instance index/generation pairs. Existing full-resolution
RGBA16F color remains unchanged in size; all three histories now total
175.781250 MiB with five instances. Alignment and resize overlap are excluded.
Resources exist only while SSR is enabled.

The temporal shader reads nine ray records with their raw samples, receiver
depths and normals, then for each of at most two candidates up to 12 history
taps of geometry and colour (four for a rough receiver), about 288 bytes
against the former four taps' 160, plus identity only for moved instances;
it writes three outputs. The trace reads the filtered roughness and, when
bound, the anisotropy plane, and no longer the five cone samples. These are
source counts; the evidence below records the measured cost. Moving
receiver shading to composite removes temporal material/LUT
reads. Graph bindings 17/18 supply hit metadata and prior transforms;
`gbuffer_anisotropy` reaches trace binding 10 and temporal binding 19 when
anisotropy is present, and both roots carry it and `sample_phase` in former
reserved bytes. Shared `VkrSsrParams` remains 288 bytes; a temporal-only
128-byte camera record contains inverse current view and previous view.
Native roots and compiled layout checks remain owned by
[ADR-044](044-shader-cross-backend-contract.md). `ssr_reflection` capture
version 6 denotes incoming radiance whose alpha is the lobe's hit share,
before the composite's roughness fade; version 5 included the fade and
version 4 was shaded RGB. Raw capture version 3 holds one lobe sample with
its edge and depth confidence; version 2 held the roughness-scaled mirror
sample.

The accepted 128-frame SSR settling period and following 128-sample scene-static
accumulation remain under [ADR-037](037-portable-same-resolution-temporal-antialiasing.md).
Portable TAA caps ordinary history retention at 90% during settling, and the
static integral continues from the ordinary history's effective sample count
instead of restarting. FSR retains its policy; reflected-hit rejection must
work before that final reconstruction stage.

## Consequences

Reflected-object correspondence addresses the wrong-surface trail that retention
caps alone cannot align. Incoming-radiance history lets the current receiver own
material response even when a reflected feature moves across that surface.
A mirror's known miss, and rough misses that refute a covered lobe, return
to probes immediately, which can expose fresh-sample noise. A glossy or
mirror reflection without current hits, whether its rays ran out of
decisions behind an occluder or a sparse hit missed, fades within four
frames. A rough reflection keeps its hit share between sparse hits; when
its source leaves, faint rough coverage (below about 15 %) fades with a
time constant of about 20 frames. On `ssr_tier_bistro` the rough wall's
covered share in `ssr_reflection` rose from 55 % to 78.5 % with unchanged
final colour (evidence below). The clamped Catmull-Rom history no longer
overshoots at edges, so it sharpens them less, and rough receivers resolve
bilinear history, slightly softer. Rough all-miss pixels, such as rough
surfaces that reflect the sky, run the surface-candidate reprojection and
read the four inner history taps. The 2026-10-11 rework raised the GPU
cost of SSR: after the 2026-10-12 restructure `SSR.Temporal` still takes
about twice its earlier time at 1280×720 in local measurements (evidence
below). At
grazing views SSR follows the stretched GGX lobe while the environment
prefilter keeps its `n = v = r` approximation, so a small mismatch remains;
on brushed surfaces SSR reflections are elliptical and probe reflections
circular. Screen-space visibility, finite ray budget, subpixel normal
variation and tangent-plane approximation still limit the result. This does
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

The fixed {-2, 0, 2} parity lattice of nine raw taps kept the rough filter's
width but sampled the same pixel classes every frame, and its mirror-ray
trace with a five-tap cone filter approximated the lobe by blur. The rotated
spiral covers a contiguous footprint with the same nine rays, and lobe
samples with ratio weights converge to the lobe's integral. Revision-based
invalidation dropped all history on every texture upload and publication
while assets streamed; the per-pixel tests already reject the pixels that
changed.

Holding only the unknown share at 0.95 per frame without a floor, the
2026-10-11 rule, faded a reflection whose rays exhaust behind an occluder
over dozens of frames and dropped a rough history on every all-miss frame,
so a small lamp's sparse reflection blinked. Halving a rough lobe's
coverage on every all-miss frame, the first 2026-10-12 rule, kept it near
30 % of its hit share and often below the 1/16 floor between sparse hits,
which left holes on rough walls. Counting every lobe's all-miss frame as a
zero sample, glossy lobes included, brought back the discs on the glossy
bar of `ssr_tier_bistro` and left 30 to 60 pixel equivalents of red at
frame 8 of `ssr_reflected_hit_motion`; only rough lobes use it.

On the RX 6700 XT a workgroup-memory tile of the reuse neighbourhood
(14×14 texels per 8×8 group) made `SSR.Temporal` 23 % slower and changed
its output at rounding level, and loading all six images of a history tap
before its branches was 0.7 % slower; both were removed. A rounding-level
bundle (no transport for unmoved receivers and hits, hit points from a
per-pixel camera-ray basis, table-driven Vogel offsets, an isotropic GGX
term for one-width lobes) saved at most 2.5 % and was not applied.

Restarting 1/64 pixel past every crossing also removes the stall, but it skips
the start of every cell and shifts every entry hit. On the Bistro bar turn it
changed 14% of raw trace pixels and lost 30-45 hits per frame. Moving 1/64 pixel
along the exit axis instead would carry a slow minor axis dozens of pixels along
the major axis and skip cells.

## Evidence and remaining checks

The SSR, TAA resolve, SSGI and deferred-lighting modules pass `spirv-val
--target-env vulkan1.4 --scalar-block-layout`, and `spirv-dis` member
offsets match the host static asserts; no 2026-10-12 change moved a root
offset or binding. `tools/checks/check_ssr_stability.py` passes 416
outputs, including reversed-depth rows for a 0.1 m to 20 km projection:
reprojected u within 1e-3 px at 1280 px and depths within 0.1 mm, rows that
fail with the former far-plane unprojection point. Its rows cover the
no-hit hold (a mirror's known miss and unknown hold, plausible and refuted
glossy all-miss frames, the four-frame end, rough zero samples without a
floor and refuted rough coverage with it), the cubic bound at a coverage
edge and the predicted plane separation. Reverting the hold, its floor, the
cubic bound or the predicted separation fails some rows, and so does each
of four mutations of the rough rule: the brief hold for every lobe, zero
samples for every lobe, refuted rough coverage without the floor and rough
zero samples with it. The later fetch changes (one fetch per receiver, 12
taps, bilinear rough history) live in the native fetch, which the oracle
does not run; its output stayed identical. The hit-direction origin has no
row. Five Bistro
cases (`ssr_ghost_under_bar`, its `_taa` and `_reference` variants,
`ssr_reflected_hit_flicker` and `ssgi_bistro_remaining_flicker`) render at
`render_scale` 1, the scale Vulkan's spatial path accepts;
`check_ssr_reflected_flicker.py` scales its regions with the capture width.

Native Vulkan runs of 2026-10-12 used Windows 10 19045, a Ryzen 5 2600 and
an AMD Radeon RX 6700 XT (driver 26.6.3), the Release `build_release`
harness from a dirty tree on `4eaeecbb`, graphics validation variables
unset except where stated, one harness process at a time and
`build_release/tools/vkr_harness.exe snapshot --case
tools/cases/local/<case>.case.json --profile tools/profiles/local-offscreen.json`.
Every report is local and not authoritative. Values come from the final
tree unless stated.

- `ssr_reflected_hit_motion` (TAA off) runs over
  [`ssr_reflected_hit_mirror.scene.json`](../../assets/scenes/fixtures/ssr_reflected_hit_mirror.scene.json),
  a copy of `ssr_shaded_layers.scene.json` whose floor under the emitter
  uses [`mirror_floor.mt`](../../tests/fixtures/rendering/ssr/mirror_floor.mt)
  (roughness 0, stored at the 0.04 mirror threshold). On the anisotropic
  floor 17 to 30 lobe hits scattered over 80 to 120 pixels, so their
  centroid error, about 8 pixels, exceeded the emitter's motion of about
  2 pixels per frame, and the "moved right" assertion failed in every
  replay. [`check_ssr_reflected_hit.py`](../../tools/checks/check_ssr_reflected_hit.py)
  requires red in `ssr_raw` and `ssr_reflection` at frame 3 that moves
  right at frame 4, no red in `ssr_raw` at frame 8, and at frame 8 in
  `ssr_reflection` at most four pixel equivalents of red energy (red
  radiance times coverage over the median red radiance of `ssr_raw` at
  frame 3) and no red pixel above coverage 0.5. All six replays pass every
  assertion, and their `ssr_raw` and `ssr_reflection` captures are
  byte-identical (frame 3 `ssr_reflection` digest `01c8fd9e9a4e`): frame-8
  residue 0.113 pixel equivalents, largest red coverage 0.125, reflection
  centroid 192.0 → 192.7. With the completed-predecessor history of the
  same day two of six replays passed: frame 4 kept frame 3's trailing edge,
  or red stayed at frame 8 (11.9 and 16.1 pixel equivalents). Captures with
  the 2026-10-11 hold, on the anisotropic floor, fail the frame-8 rule with
  16.4.
- `ssr_tier_bistro` (640×360, TAA on) passes with finite `ssr_reflection`
  and `hdr_pre_bloom`, alpha in [0, 1] and 56.4 % of the frame covered. The
  0.95 hold of 2026-10-11 stamped 5 to 10 pixel discs on the rough bar
  front and stools (report `sha256:1f50003b...7246`). The first 2026-10-12
  rule removed them but lowered the rough wall's covered share from 81 % to
  55 %; the rough zero sample raised it to 78.5 % with wall `hdr_pre_bloom`
  mean luminance 0.06264 → 0.06261 (report `sha256:873446b7...595ac6`). The
  bilinear rough history then lowered the wall's coverage-weighted reflection
  luminance 3 % with `hdr_pre_bloom` unchanged to five digits. Discs and
  holes stayed out of final colour, inside the roughness fade.
- `ssr_ghost_under_bar_taa` passes in five replays with
  `post.screen_history.state` 1 on all 56 measured frames of each (report
  `b8d94ed328f8`). Its final colour shows a grainy, dithered reflection of
  the plate stack on the glossy bar top after 28 still frames; no earlier
  capture of that view exists.
- `ssr_reflected_hit_flicker` with `check_ssr_reflected_flicker.py` passes.
  Mean `ssr_reflection` RGB range over checkpoints 0/1/4/8 fell in every
  region from the first Vulkan run at `render_scale` 1 (2026-10-11 rules,
  report `sha256:15fee786...f5c6`): bar lip 0.0252 → 0.0190, bar panel
  0.00344 → 0.00280, counter 0.0300 → 0.0250, upper gloss 0.0523 → 0.0322;
  `hdr_pre_bloom` ranges moved by at most 0.0001.
- API validation (`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`, core and
  synchronization validation enabled by a `VK_LAYER_SETTINGS_PATH` file,
  `--profile tools/profiles/local-offscreen-gpu-single.json`) reported no
  message for a copy of `ssr_ghost_under_bar` (TAA off) with longer
  timeouts, 64 + 56 frames at 1509×907 with history selected on every
  measured frame (report `sha256:7c749f14f425...`), and for
  `ssr_reflected_hit_motion` with the final shaders (`dc6b86eab05b`).
  Profile runs take no captures, so the capture copy's layout restore is
  not validated.

Cost, local observations and not a speed claim:
`build_release/tools/vkr_harness.exe profile --case
tools/cases/local/<case>.case.json --profile
tools/profiles/local-offscreen-perf-audit-gpu.json` with per-pass GPU
timestamps and two children per run, on `ssr_tier_bistro_cost` (640×360,
TAA on, SSGI off, 24 warmup and 32 measured frames) and
`ssgi_bistro_{off,on}_profile_local` (1280×720, TAA and SSR on, 24 and
16 frames). Against the same tree with the whole uncommitted change set
stashed, the rework raised `SSR.Temporal` from 0.170 to 0.702 ms at
640×360 and from 0.68–0.74 to 2.51–2.52 ms at 1280×720,
`SSR.Trace.Fullscreen` by 15 to 22 % and `Temporal.Resolve.Fullscreen` by
15 to 18 %; frame wall grew 18 to 28 %, all of it GPU work, with equal
work-volume rows (640×360 reports `sha256:b6373852...` before and
`sha256:a6b37e05...` after). After the 2026-10-12 restructure, in three
alternated runs per build with medians reported, `SSR.Temporal` took
0.457 ms at 640×360 and 1.52 ms at 1280×720, and frame wall 3.21 ms and
9.51 ms (SSGI off) or 10.13 ms (SSGI on), against 2.92, 8.23 and 8.80 ms
before the rework. One history fetch per receiver with the shared plane
gave 27 to 30 %, the 12-tap cubic 7 to 9 % and bilinear rough history 2 to
7 % of `SSR.Temporal`. Every report has `authoritative=false` with reasons
`profile.local_only` and `provenance.dirty`; the before tree ran camera
script version 6, so workload fingerprints differ by that version while
every work-volume row matched, and the last comparison spans two sessions.
An authoritative comparison needs a clean tree and a performance profile;
`local-offscreen-gpu-repeated.json` rejects these cases' 24-frame warmup.

Still unverified: a clean-tree authoritative timing; the TAA seed and the
settling switch ([ADR-037](037-portable-same-resolution-temporal-antialiasing.md));
`post.screen_history.state` on harness upload frames (the
`vulkan_texture_stream_features` case uploads nothing in its recorded
frames; Bistro in the editor kept history 257 with 426 to 496 textures
pending) and while point lights enter the table (no Bistro case has more
than 128 lights); the indoor fade (no compatible Bistro case);
`ssr_reflected_hit_editor`, which cannot run on Vulkan because its
`render_scale` 0.8 is Metal-only. The first `ssr_tier_bistro` child after a
shader change ran past the 120 s case timeout during cold pipeline creation;
the rerun took 96 s.

The restart correction extends `tools/checks/check_ssr_stability.py`. Before
it, a 3.84-pixel segment re-entered 14 cells, and a nearly horizontal ray at a
rounded row boundary needed 69 steps against the 48-decision budget. Other new
cases keep the epsilon restart where it already clears the boundary and check
the 1/64-pixel distance. All 328 outputs pass. On the Metal desktop
implementation, removed on 2026-10-06 (Release, M1,
`tools/profiles/local-offscreen.json`), `local/ssr_bar_turn_stop` gained
228-253 trace hits per captured frame, about 0.4% of 63k, and lost at most 2.
`local/ssgi_bistro_reported_turn_stop` gained 8,014-10,284 SSR hits of
505k-532k and lost at most 4; both are Metal desktop cases since removed. The
recovered hits formed vertical stripes along rounded pixel columns on the
ceiling. Neither case captured the SSGI trace, so SSGI is covered by the shared
code only. Vulkan compiles and its SPIR-V validates; native Vulkan has not run
the correction.

The full-resolution trial ran on the Metal desktop implementation with the
same later bar camera, four checkpoint replays and 80% spatial/TAA settings as
the radiance-owner correction below. Keeping the
former rough filter width reduces mean final display-code range on the bar lip
34.5%, from 0.80762 to 0.52936 out of 255. Mean peak display code changes
39.9236→40.2081; pixels changing by more than eight codes fall 407→108.
Countertop and upper woodwork ranges fall 5.6% and 13.3%; the panel range rises
2.6%, and some newly resolved lamp pixels fluctuate more. This is a partial
improvement, not a flicker-free result. The immediate-neighbor full-resolution
trial reduced bar-lip range only 24.6% and is superseded by the wider footprint.
Commands, payload digests, native checks and cost limits belong to the
[full-resolution tracing record](../../assets/verification/renderer-features/ssr-full-resolution-tracing.txt).

The radiance-owner correction was measured on the Metal desktop implementation
with the user's later bar camera at 1784×1093 output, 80% spatial scaling, TAA
enabled and SSGI disabled. Across checkpoints 0/1/4/8, mean coverage-weighted
peak-RGB range falls 26.4% on the bar lip, while mean light rises 15.0%. The
previously worst lamp pixel's TAA-output range falls 93.5%; the whole bar-lip
TAA range falls 17.6%. Final display-code range falls only 2.7%. These
sampled-phase measurements do not establish flicker-free motion: thin edges and
absent current hits still shimmer. The unchanged rejection checks pass the
moving-emitter disappearance case. Release app/editor builds, 290 shared-math
outputs and temporal SPIR-V validation pass. [The radiance-owner
record](../../assets/verification/renderer-features/ssr-radiance-owner.txt)
owns commands, payload measurements, cost limits and the remaining coverage.

The original reflected-hit implementation has the following evidence:

Release app and editor builds pass. The shared production Slang check passes
268 independently expected outputs within 1e-6, including the exact planar
previous UV 19/30 where receiver motion gives 0.65, a 21.33-pixel error at width
1280. It also covers jitter, affine and mirrored transforms, moving receivers
and hits, orthographic projection, depth/normal/identity rejection and current
receiver shading. Eleven SSR/SSGI/deferred SPIR-V modules validate with scalar
block layout; compiled reflection confirms the 512-byte Vulkan temporal root,
128-byte camera record and 80-byte retained transforms.

On the Metal desktop implementation, at the user's Bistro under-bar camera, an
18-frame in/out movement followed by a hold produced less residual reflection
error. With TAA and SSGI disabled, raw SSR is byte-identical before/after at
all five checkpoints. At the first held frame, pre-TAA HDR error against each
run's frame 48 falls 64.5% on the countertop and 51.1% on the bar front. Frame
48 is a settling reference, not ground truth; the bar-front frame-32 error
rises 6.1%. With TAA enabled, comparison to each revision's separate
320-warm-frame reference reduces countertop error 27.2% at the first held frame
and 12.7% at frame 48; bar-front reductions are 6.2% and 3.5%. These are
regional convergence measurements, not exact ghost masks.

The moving-emitter fixture exercises a stationary receiver with a moving
reflected object and TAA disabled. On the Metal desktop implementation, its red
reflection shifted in the expected direction, then left zero red history pixels
in the checked floor region when current support disappears. Other reflections
remain covered. Layered-material and odd-size scaled editor captures have
finite incoming radiance, nonzero reflections and coverage within [0,1]. The
separate SSGI flicker camera's mean display-code range changes only 0.62% over
four unsettled checkpoints; this does not establish flicker-free output.

Matched local Release cost observations used the Metal desktop implementation,
removed on 2026-10-06, on Apple M1 Pro: source 1025×577, output 1280×720
through the removed MetalFX upscaler at 80%, three repetitions of 120 measured
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
a screenshot. The Windows SSGI investigation found that Vulkan SSR composite
shared its zeroed-frame-root pattern; the host now fills materials, instances
and frame constants before that dispatch. Release shader compilation and root
reflection pass. The mirror witness scene now uses a black constant
environment instead of an untracked HDR file, but no native Vulkan run has used
it. Vulkan is the only SSR implementation (desktop class, ADR-087), so its
native evidence must come from Vulkan runs; the Bistro cases above ran on
2026-10-12, and the Vulkan-pinned mirror case (`ssr_mirror_on_local`) has
not run.

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
cannot numerically validate version-5 incoming-radiance history, and neither
can validate version-6 coverage or version-3 raw samples.

## Revisit when

Revisit traversal resolution, step count or filtering only with matched output
and frame-cost evidence. Selective planar reflections remain separate work for
surfaces that need reliable off-screen geometry. If `SSR.Temporal` must
cost less than its measured 1.5 ms at 1280×720, fewer reused rays for rough
receivers (eight to four at roughness 0.4 and above) is the next lever,
unmeasured so far; it adds noise to rough
reflections, so measure the flicker regions first. If a Bistro view shows
faint rough reflections lingering after their source leaves, revisit the
rough zero sample's retention.

## Code evidence

- [Portable controls and parameters](../../renderer/src/vkr_ssr.h)
- [Shared traversal and filtering](../../renderer/src/shaders/shared/ssr_kernel.slangh)
- [Authored graph](../../assets/render_graphs/main.rendergraph.json)
- [Frame controls](../../renderer/src/vkr_frame_input.h)

The [2026-09-12 local implementation evidence](../../assets/verification/renderer-features/renderer-features-perf.txt)
records high/balanced coverage and pass-cost observations. These are different
quality workloads on the Metal desktop implementation (M1 Pro), removed on
2026-10-06, without authoritative budget acceptance.
