---
status: implemented
updated: 2026-10-12
authority: adr
---

# ADR-060: Independent screen-space diffuse indirect lighting

## Status

Accepted. SSGI belongs to the desktop pipeline, and Vulkan is its only
implementation; the tiled pipeline turns it off
([ADR-087](087-gpu-class-graphics-pipelines.md), decision 7). Production graph,
native commands and completion-protected history ownership are integrated.
Native Vulkan execution passed on Windows on 2026-09-12; output evidence for the
history correction comes from the Metal desktop implementation, removed on
2026-10-06. The 2026-10-11 revision (sequence, trace start, filter weights,
history rule and composite coverage below) and the 2026-10-12 changes (plane
tolerance, history placement allowance, all-miss and unanswered decay,
content signature, the per-sample firefly bound, the exact-predecessor
history without final TAA and the per-group tiles) pass CPU checks and
native Vulkan runs on one Windows host from a dirty tree, recorded below
as local, non-authoritative evidence.

## Context

Baked diffuse volumes provide static multi-bounce lighting but cannot follow all
dynamic emission or lighting changes. Screen-space diffuse tracing can supplement
uncovered regions, with explicit limits on off-screen information and work.

## Decision

Offer SSGI as an optional mode, disabled by default and independent of SSR and
final TAA. Trace at half resolution with one cosine-weighted ray per receiver
and at most 24 hierarchy decisions. A separate current-frame R32F depth hierarchy
provides visibility. Deferred lighting writes a full-resolution RGBA16F source
containing layered direct punctual/rectangle diffuse light and emission.
Camera-directed base specular, coat and sheen highlights are excluded, as are environment,
probe and baked-volume indirect light, SSR, fog and post-processing.

The graph owns seven image families: the direct source, half-resolution depth
pyramid, half-resolution RG32UI receiver metadata, half-resolution raw RGBA16F
radiance, and a retained color/depth/identity
tuple in RGBA16F, R32F and RG32UI. Current images have one instance per frame slot;
each history member has frame-slot count plus two instances. Three frame slots
require 27 images and 57.129 MiB at 1280×720, before allocator alignment.
Receiver caching adds 5.273 MiB to the preceding 51.855 MiB three-slot budget.
The graph owns its current-frame lifetime and orders depth-base writes before
trace, temporal and composite reads; history ownership is unchanged.

Select the nearest covered receiver once while building depth mip zero. Store
positive view depth and the 0–8 local offset as bit-preserved float values in
RG32UI. The offset identifies the exact source pixel even in the final 3×3
odd-dimension footprint; zero depth denotes an uncovered cell. Later passes
read this metadata instead of rescanning full-resolution depth/visibility. Each valid receiver traces one
cosine-weighted direction from a deterministic 256-phase Halton(2, 3)
sequence (`sample_phase & 255`) with a per-pixel Cranley-Patterson rotation
(`vkr_ssgi_sample_2d`): any 2^a × 3^b consecutive phases fill each
2^-a × 3^-b cell of the square once, so the temporal average's short window
covers the hemisphere. SSR samples its GGX lobes with the same sequence
([ADR-055](055-screen-space-reflections.md)). The trace starts at the
receiver's own hierarchy leaf and ascends one level per advance; the former
top-level start only descended through cells that all contained the origin.
The hit slab's half-width is the configured thickness or four source-pixel
footprints at the surface depth, whichever is larger (`vkr_ssgi_thickness`;
0.1 m near, 0.74 m at 100 m for 1080 rows and a 90 degree view). A valid
screen miss writes zero radiance with alpha one and remains part of the
estimator; a ray still active after 24 decisions writes alpha zero and is
excluded.

The temporal pass applies a 3×3 raw spatial filter before history clamping. It
loads each cached nearest covered neighbor and weights it by its distance to
the centre receiver's tangent plane, against the larger of the thickness at
the centre's depth and a quarter of the separation that plane predicts at
the neighbour's pixel, by a normal term rising from a dot product of 0.5,
and by 1/(1+d²) (`vkr_ssgi_plane_weight`). The predicted separation is the
distance from the centre to where the neighbour's camera ray meets the
plane, capped by the measured separation (`vkr_ssr_plane_separation`, shared
with SSR, [ADR-055](055-screen-space-reflections.md)). Grazing floors
therefore filter across distant neighbours, and steps and creases do not; a
floor seen past a counter's edge cannot widen its own tolerance.
Each raw sample in the 3×3 gather is first scaled down, hue kept, to the
larger of four times its reference luminance and the luminance whose share
of the filtered estimate adds a quarter of displayed luminance
(`vkr_ssgi_firefly_clamp`, `VKR_SSGI_FIREFLY_DISPLAY_SHARE`). The reference
is the larger of the other samples' weighted mean luminance
(`vkr_ssgi_other_mean_luminance`) and the receiver's accepted history
luminance, so a lone outlier cannot raise its own bound. The share is the
sample's weight over the neighbourhood's. The temporal root carries display
exposure over pre-exposure, since pre-exposure stays 1 within four stops
while the harness cases use manual exposures from 0.1 to 8
(`VkrExposureFrame.display_exposure`: the manual exposure, or the newest
completed automatic one). A lone hit on
a small emitter therefore adds at most a quarter of display white to any
receiver, an isolated one included; dense bright bounce raises the other
samples' mean and bounce that persists raises the history, so both pass.
The clamped samples also give the mean and variance of the history box.
Valid misses remain zero-valued samples in the normalized average.
Non-finite raw and history values are skipped, and half-float stores drop
non-finite values and saturate at 65504.

Each 8×8 temporal group first writes, for every trace texel of a 10×10
tile, the raw sample and the receiver's camera ray, depth scale and view
normal to workgroup memory; the filter's taps read the tile and form the
position from the ray and the scale. Composite does the same with a 6×6
tile per 8×8 pixel group (receiver ray, depth scale, view normal and
temporal sample) for its bilinear and silhouette taps, and evaluates a tap
outside the tile, possible only with another trace-to-source ratio,
directly. Storing the two factors instead of the position keeps the
result bit-identical to evaluating each tap in place.

History is the color/depth/identity tuple matching the transform that supplies
motion: its instance, submit value, frame, scene generation, SSGI content
signature (lighting without the point-light table, sun-shadow configuration
and the local-shadow settings, map size and face budget;
`vkr_ssgi_content_signature` in
[`vkr_temporal.c`](../../renderer/src/vkr_temporal.c)), graph revision,
dimensions and retained shadow state must match. Texture uploads, asset
publications, pending assets, world content generations and local-shadow
strength fades do not invalidate it (user approval, 2026-10-11): the
per-tap identity and depth tests reject changed geometry. Since 2026-10-12
neither do point-light table changes nor the resident shadowed lights' views
and strengths. Both follow the camera: the scene table ranks point lights by
camera distance and fades a light in or out when its selection changes, and
the shadow cache's residents and strengths follow the distance fade and the
atlas budget ([ADR-019](019-bounded-forward-spatial-lighting.md)). The
history box limits the light a change adds or removes. Portable TAA's own
signature still hashes every point light.
`post.screen_history.state` reports the selection and the reason for a
rejection every frame ([ADR-055](055-screen-space-reflections.md)). Vulkan's existing
same-queue image barriers order the shared predecessor before CPU-observed
completion. With final TAA off the shared predecessor is still the
immediately preceding frame, so SSGI history never skips a submitted frame;
no other producer is eligible. The temporal pass
reprojects the unjittered motion coordinate onto the raw grid by adding the
producer's previous-minus-current raster jitter. FSR derives that jitter from
its active upscale sequence length; non-FSR paths use the fixed eight-phase
sequence. No-TAA leaves the offsets zero.

Four bilinear history color taps independently validate identity and depth.
A tap's expected previous depth is the receiver's previous depth scaled by
the depth of the receiver's tangent plane at that tap, moved back by the
receiver's motion, over the receiver's depth
(`vkr_ssgi_history_tap_accepted`). The allowed error adds a placement
allowance to the absolute and relative limits, since the stored receiver
lies anywhere in its 2×2 cell: half the largest change of the plane's depth
from the receiver to its four axis neighbours one trace texel away, at the
previous depth's scale. A probe past the plane's horizon counts as the
receiver's depth, which also caps the change
(`vkr_ssgi_texel_depth_change`), so near a vanishing line the allowance
covers the side where the plane recedes and is never zero. Their RGB values
resolve with bilinear weight multiplied by each tap's history confidence.
The temporal weight is the configured weight (0.9 by default) times the
accepted bilinear support.

In a neighbourhood whose answered rays all missed, history keeps that
weight while every bilinear tap supports it, for a static receiver and for
a camera pan over static geometry alike, since one all-miss phase of a
sparse hit field is a valid zero sample. Where support is partial, as where
motion disoccluded part of the footprint, an extra factor falls linearly
from 1 at full support to 0.6 without support (`vkr_ssgi_empty_retention`);
screen motion itself does not gate it. A receiver without an answered ray in its 3×3, because every ray
exhausted its decisions or no receiver there traced, keeps accepted history
at 0.95 per frame (`VKR_SSGI_UNANSWERED_RETENTION`), so stale bounce cannot
stay forever.

Composite the diffuse residual after deferred lighting and before SSR. Apply
receiver diffuse energy, albedo and material AO once. Do not apply GTAO visibility
or multi-bounce compensation to this new traced path. A pixel that deferred
lighting lit from the baked volume receives no SSGI: composite reads
deferred's `diffuse_volume_irradiance` (graph binding 14) and skips a pixel
whose alpha is positive, one storage read in place of the full volume lookup.
The existing bake remains authoritative there. Upsampling uses the plane
weight; when all four bilinear texels fail, the best plane and normal match
in the 3×3 trace footprint around the pixel's own trace cell supplies it,
at that texel's confidence. Non-finite texels are skipped. SSR can then
reflect the completed opaque diffuse lighting.

## Ownership and lifetime

The public frame borrows an enable flag. Renderer preparation owns normalized
quality controls, the 256-phase index, and temporal invalidation. The graph owns
image dependencies; the Vulkan backend realizes storage and retains
completion-proven history tuples. Failed or canceled submissions do not publish
new history.

The temporal transform predecessor is the immediately preceding frame in
every mode: portable TAA, FSR and, with final TAA off, the tuple SSR, SSGI
and motion blur read
([ADR-037](037-portable-same-resolution-temporal-antialiasing.md)).
SSGI consumes its exact matching tuple through the existing
queue dependency, even when the GPU has not completed it. An older completed
tuple cannot replace that motion reference. When no compatible predecessor exists,
SSGI sets history invalid and uses the current one-ray estimate without waiting or
recreating images.

## Consequences

SSGI adds dynamic screen-visible diffuse bounce outside valid baked-volume cells.
It cannot recover off-screen emitters or occluders and does not replace baked
multi-bounce lighting. Low ray count requires spatial and temporal filtering; changed content or a
missing compatible predecessor can temporarily lose accumulated detail. Storage and
traversal costs are explicit and remain optional.

The firefly bound biases rare bright contributions: a source brighter than
about a quarter of display white that fewer than about one in four rays of
an isolated receiver see is limited by the display share, and faint tints
remain where a few rays per frame hit a bulb. On `ssgi_bistro_on_local` the
bias stayed in outlier pixels (evidence below). The extra all-miss
decay applies only where accepted history support fell, because decaying
supported history darkens sparse hit fields (at a 5 % hit rate, 63 % of 3×3
neighbourhoods have no hit), also during a camera pan; a bright object
leaving a static view, or a bounce source leaving the screen during a pan,
fades at the temporal weight, about 22 frames to a tenth. The placement
allowance accepts more history taps near vanishing lines, where identity
still has to match. The predicted plane separation follows the shading
normal, so a normal-mapped normal tilted toward the camera shortens it and
far normal-mapped ground filters across fewer neighbours. The two-sided
placement probes add two plane evaluations per receiver, the predicted
separation about ten arithmetic operations per filter and composite tap,
and the firefly bound a second pass over the nine samples; the per-group
tiles share the neighbours' positions and normals. A trace from the
receiver's leaf marches further within the same 24 decisions, so its cost
can rise; the composite's coverage read replaces the full volume lookup
(8 probe records, 8 moments and 48 SH texels). Measured locally at
1280×720, the SSGI passes cost about 0.12 ms more per frame than before the
2026-10-11 revision (evidence below).

## Alternatives considered

SSR and SSGI use matching minimum-positive-depth reduction and odd-tail
coverage, so the hierarchy is mathematically shareable. The graph currently
retains independent producers and feature conditions: caching receiver selection
does not introduce an SSR prerequisite or change traversal policy. Sharing
storage would also require conditional producer/resource routing when either
feature runs alone; it remains a separate optimization. GTAO reduction differs
and cannot substitute for this hierarchy.
Tracing the final opaque HDR would bounce existing indirect light again. Lowering
source precision or resolution could reduce storage, but those quality changes
are outside the accepted first scope.

## Revisit when

Native measurements require a lower budget, or dynamic bounce inside valid baked
coverage needs a different composition policy. If far normal-mapped ground
loses its filter, take the plane weight's normal from depth instead of the
shading normal. If a daylight view shows the firefly bound's bias, raise
the history multiplier first: a source that at least one in K rays sees
then converges unbiased.

## Implementation and verification

[Shared controls and GPU parameters](../../renderer/src/vkr_ssgi.h), the
[graph declarations](../../assets/render_graphs/main.rendergraph.json), and the
five native phases are integrated: depth base/mips, trace, temporal, and
composite. Deferred lighting writes the isolated direct/emissive source only
when SSGI is enabled.

`VkrSsgiParams` remains 288 bytes. Its former unused tail at bytes 280 and 284
is `history_jitter_uv_x/y`; no root or image allocation changes. Byte 252,
formerly reserved, is `pixel_footprint`, `2 / (|P[1][1]| × source height)`,
zero when not finite. The composite root stays 448 bytes; byte 436,
formerly reserved, is `diffuse_volume_texture` (graph binding 14). The
temporal root stays 368 bytes; byte 364, formerly reserved, is
`display_exposure_scale` (2026-10-12), which the host fills with display
exposure over pre-exposure, or 1 when that is not finite and positive
(`vkr_vk_prepare_ssgi_temporal`). The Vulkan temporal shader implements the
four-tap, confidence-weighted reconstruction and exact-predecessor contract.

The SSGI, SSR and deferred modules pass `spirv-val --target-env vulkan1.4
--scalar-block-layout`, and `spirv-dis` member offsets match the host
struct and reflected table. The
[`check_ssgi_kernel.py`](../../tools/checks/check_ssgi_kernel.py) check runs
the production kernel through Slang's CPU target and passes 38 outputs with
expected values from stated geometry: sequence strata and period, the
firefly bound (a lone bulb hit, dense bright bounce, dim sparse bounce, hue,
a lone sample, an isolated receiver, the history reference, exposure 8),
plane weights on a grazing floor, a step, a crease and a floor past a
counter's edge, per-tap expected depth, the placement allowance near the
plane's horizon, the far-field slab, non-finite storage, the all-miss
retention by history support, the unanswered fade and the leaf start.
Deliberate regressions fail it: a linear sequence, receiver-depth history,
a fixed slab, no firefly bound and a top-level start; reverted one at a
time, the measured-separation tolerance, a motion-gated decay, a one-sided
placement probe and unanswered history kept unchanged; and five firefly
mutations: the bound including the sample, the share without exposure, a
floor without the share, no relative term and no share term.
`check_ssr_stability.py` passes with the plane weight and thickness
arguments. The per-group tiles leave both checks' output unchanged.

Native Vulkan runs of 2026-10-12 used Windows 10 19045, a Ryzen 5 2600 and
an AMD Radeon RX 6700 XT (driver 26.6.3), the Release `build_release`
harness from a dirty tree on `4eaeecbb`, graphics validation variables
unset except where stated, one harness process at a time and
`build_release/tools/vkr_harness.exe snapshot --case
tools/cases/local/<case>.case.json --profile tools/profiles/local-offscreen.json`.
Every report is local and not authoritative.

- `ssgi_bistro_on_local` against `ssgi_bistro_off_local` (1280×720, TAA
  and SSR on, `hdr_post_transmission` luminance, SSGI on minus off): under
  the bound that included the sample, coloured splats covered the
  cobblestones under the string-light bulbs, which reach about 1,830 in HDR,
  and the result equalled a build without any bound (223 street pixels with
  an addition above eight times the SSGI-off luminance). With the
  per-sample bound the street has 0 such pixels and the whole frame 45 to
  75 instead of 624, and the largest addition fell from 20.6 to 4.42. On
  the left facade the median of 10×10 block-mean additions equals the
  unbounded reference; the mean is 3.7 % lower, 91.5 % of that difference
  in the brightest 1 % of pixels (reports `sha256:dd8c09e9...2762` and
  `sha256:98895987...9f1c`, then with later SSR changes
  `sha256:e5b0ef58...2e531b` and `sha256:4ebd3129...d5434`). The
  per-group tiles keep `depth`, `final_color` and `hdr_post_transmission`
  byte-identical.
- `ssgi_bistro_remaining_flicker` (manual exposure 8) passes in four
  replays with `post.screen_history.state` 257 on every measured frame. At
  frame 8 the mean SSGI addition is within 7 % of the unbounded reference,
  all of the difference in the brightest 1 % of pixels; 3,207 pixels change
  by more than eight display codes over checkpoints 0/1/4/8, against 3,163
  with SSGI off and 3,482 unbounded (report `sha256:b734464f...293347e`).
- History across uploads: Bistro opened in the headless Release editor on
  the same host kept SSR and SSGI history (257) in the `--metrics-json`
  exit frame 1 s and 4 s after `wait.scene`, with 496 and 426 textures
  pending. `vulkan_texture_stream_features`
  (`--profile tools/profiles/validation-windowed.json`, report
  `sha256:816377a9...ae82`) cannot show it: its recorded frames upload
  nothing, and its measured frame 4 reads 4626 (reset and no tuple for
  both) after the case's resize round trip.
- With API validation (`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`,
  core and synchronization validation enabled by a `VK_LAYER_SETTINGS_PATH`
  file, `--profile tools/profiles/local-offscreen-gpu-single.json`),
  `ssgi_vulkan_validation` reported no message with the final shaders
  (report `sha256:357b2955...`).

Cost, local observations and not a speed claim:
`build_release/tools/vkr_harness.exe profile --case
tools/cases/local/ssgi_bistro_on_profile_local.case.json --profile
tools/profiles/local-offscreen-perf-audit-gpu.json` (1280×720, TAA and SSR
on, 24 warmup and 16 measured frames, two children, per-pass GPU
timestamps). Against the same tree with the whole uncommitted change set
stashed, the 2026-10-11 revision and the firefly bound raised
`SSGI.Temporal` from 0.082 to 0.129 ms and `SSGI.Composite.Fullscreen` from
0.147 to 0.268 ms (reports `sha256:2abd906e...` before and
`sha256:08108a5f...` after). With the per-group tiles, in three alternated
runs with medians reported, they take 0.116 and 0.200 ms, and the SSGI
group 0.626 against 0.504 ms per frame before the revision. The reports
have `authoritative=false` with reasons `profile.local_only` and
`provenance.dirty`, the before tree ran camera script version 6, and the
last comparison spans two sessions.

Not yet run: sequence convergence, composite rims and the counter and sill
halo on `ssgi_bistro_on_local` checked against a reference; fireflies on a
baked Bistro night view with TAA and SSGI; the volume edge with SSGI on a
baked Bistro view (`bistro_lamps_baked_volume_night_capture` runs without
SSGI); non-finite output at high pre-exposure; mean bounce during a camera
pan within a tenth of the still frame's (no Bistro pan case with SSGI
exists); history while point lights enter the table (no Bistro case has
more than 128 lights); an authoritative clean-tree timing.

The receiver-cache and diffuse-only-source changes supersede the relevant source
and storage statements in older observations below. Production-Slang arithmetic
checks exercise nearest covered ties, uncovered samples and odd-tail offsets.
The 2026-10-12 Vulkan runs above execute them; older feature runs do not
establish their output or speed.

The history-correction revision passed Release app/editor builds, all ten
SSR/SSGI SPIR-V validation checks, compiled parameter reflection and Vulkan
host syntax. On the Metal desktop implementation, removed on 2026-10-06,
Bistro captures reduced excess displayed variation above the SSGI-off
control by 86.6% in the reported view. [The correction
record](../../assets/verification/renderer-features/ssgi-ssr-history-correction.txt)
retains commands, hashes and limits; native Vulkan ran only its 2026-10-12
successor above.
The existing source-isolation, emission and baked-volume observations remain
evidence for the SSGI source and composite policy, not for this temporal
change.

On the Metal desktop implementation, the source-isolation capture had 98,304
byte-identical HDR pixels with SSGI on/off. The emissive fixture gains red
bounce at 2,853 non-emissive opaque pixels. A valid enclosing baked-volume cell
suppresses that contribution at all 39,722 covered samples, producing
byte-identical on/off HDR. The same geometry without the volume retains the
visible bounce. Editor composition uses its own `scene_pre_transmission`
target; a 257×193 editor capture passes.

The serialized Release Bistro observations at 1280×720 on the Metal desktop
implementation (Apple M1 Pro), removed on 2026-10-06, used 24 warmup and 16
measured frames, with TAA, SSR and GTAO enabled. The added SSGI passes total
2.594 ms mean: trace 0.720 ms, spatial/temporal 0.712 ms, composite 1.000 ms,
and depth construction 0.162 ms. Live graph storage grows by 18 images and
39,321,304 bytes (37.50 MiB) with the current two frame slots. These are
single-process local observations from a dirty tree, not an authoritative
performance comparison. The profile command was
`./build_release/tools/vkr_harness profile --case tools/cases/local/ssgi_bistro_on_profile_local.case.json --profile tools/profiles/local-offscreen-gpu-single.json`
with graphics validation variables unset; the paired off case changes only
SSGI. Both cases are now pinned to Vulkan.
[The measurement record](../../assets/verification/renderer-features/ssgi-cost-numeric.txt)
retains both run identities and distributions.

Native Vulkan execution now passes on Windows with an AMD Radeon RX 6700 XT,
driver 26.6.3 and Vulkan API 1.4.315. The reported device loss exposed three
independent defects: sampled graph slots were indexed through storage descriptor
arrays, the variable trace loop needed an explicit `[loop]` policy for the AMD
compiler, and the composite pass allocated but did not populate its frame root
before reading material rows. The Vulkan shader now uses typed sampled arrays,
preserves the authored 24-step bound with explicit loop control, and the host
fills the composite root before dispatch. SSR composite used the same root
pattern and is corrected by the same ownership rule.

The Release emission on/off reports are
`20260912T103829.989Z-000bd8` (`6980163ae7bcc5a7cfe9d93c42cbbc26c6154b05ebd448fdc423e2b6eed6dd62`)
and `20260912T103835.198Z-001187`
(`21858b94498b2fe0d265a0ef6b0c0d2a172acef8d45ef8ebb56fa8fa04667ba6`).
The Release Bistro report is `20260912T103625.242Z-002637`
(`db8d1d6000d6407206037b0f718d145181c0e2e7b55bbe312211767fecc2e93c`).
Focused Debug synchronization validation is
`20260912T111516.304Z-004249`
(`8368380187d0358eadf810dfe0b0a1e187db988ade6933ac00da749dc2018ac1`).
The child loaded Khronos validation and reported no API or synchronization
errors. These dirty-tree local reports establish bounded native Vulkan
execution, not authoritative acceptance or verified output.
