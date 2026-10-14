---
status: implemented
updated: 2026-10-14
authority: adr
---

# ADR-019: Bounded punctual lighting, local probes, and cached local shadows

## Status

Accepted. Both pipeline classes use the light table, the light grid, the local
shadow cache and its atlas raster. `Shadow.LocalMask`, contact shadows, local
transmission layers, contribution measurement and local probes belong to the
desktop pipeline, which Vulkan implements. The tiled pipeline draws a bounded
set of dynamic lights, and a shadowed one takes one bilinear comparison tap
([ADR-087](087-gpu-class-graphics-pipelines.md), decisions 8 and 11). Since
2026-10-09 a cached face holds static casters only and moving casters draw
into a per-frame copy of it, on both pipeline classes; the tiled pipeline's
side has native Metal evidence since 2026-10-10. Since 2026-10-11 a dynamic
square that motionless casters leave unchanged is reused, the atlas keeps to
a layer budget per preset, the table ranks lights by camera distance, and
`Shadow.LocalMask` runs only while a shadowed light reaches the camera
frustum. Native Vulkan harness runs cover the reuse and the mask's light
mask; the skipped mask, the budget and the ranked table ran only on the
user's level in the editor, and none of the four ran on Metal (Evidence).
Since 2026-10-12 probes blend in priority order with an inward blend band,
look up the dominant specular direction, carry only their own gains, and
bake with source mips and without the sky's discs, glow and stars
([Short-history rendering](../proposals/short-history-rendering.md),
section 5). Native Vulkan evidence covers the Bistro street view and the
café probe bake; the Metal side of the change was neither compiled nor run
(Evidence). Also since 2026-10-12 a baked probe follows the time of day by
the ratio of the diffuse volume's baked light at its capture origin, its
capture renders only the static world in supersampled faces, and its cube
records where it was captured (audits B4, B5 and B7). Since 2026-10-13 box
projection aims from that capture origin on both pipelines, and a probe an
edit moved off it is marked for recapture. B4 and B5 are host-side; B7 adds
a field to both pipelines' probe record. The Metal side was not run. Since
2026-10-14 the desktop pipeline with ray queries also scales a probe by the
diffuse volume's dynamic layer at its capture origin
([ADR-119](119-dynamic-diffuse-volume-layer.md)).

## Context

A scene-wide light prefix drops lights unpredictably, and selecting one probe
per draw causes large meshes to inherit the wrong local environment.

## Decision

Use a table of up to 128 punctual lights, its rows in render-id order, and a
conservative 384-cell fragment-local bitmask grid. CPU scene synchronization
owns membership; shaders perform exact range and spot-cone rejection.
Four-vector, 64-byte light rows share position/range, direction/cone,
color/intensity and type semantics across Metal and Vulkan shading paths.
When more lights are offered than the table
holds, it keeps the 128 nearest the camera by the distance to their range,
ties by the distance to their centre, and a light selected the frame before
ranks as if the camera were 1.15 times closer. While lights compete for the
table, a newly selected light fades in over 0.25 s once a slot is free and
a light no longer selected fades out in its slot; the fade scales the
table's `intensity`
([`vkr_lighting_system_select_point_lights`](../../runtime/src/renderer/systems/vkr_lighting_system.h)).
The table therefore changes while the camera moves.

Pack up to 16 ready local IBL probes per frame
(`VKR_FRAME_IBL_PROBE_MAX`). The host lists them in priority order
(`vkr_standard_scene_runtime_order_probes` in
[`vkr_standard_scene_runtime.c`](../../runtime/src/application/vkr_standard_scene_runtime.c)):
when more are ready than `ibl_probe_limit` allows, it keeps those nearest the
camera by distance to their box, a probe a planar reflector binds first
([ADR-117](117-desktop-planar-reflections.md)), then sorts the kept probes
by box volume, smallest first. The scene and the frame both hold at most 16 probes, so
today the distance rank drops probes only under a lower `ibl_probe_limit`.

The shared kernel
[`reflection_probe_kernel.slangh`](../../renderer/src/shaders/shared/reflection_probe_kernel.slangh)
gives both pipelines one meaning for influence and lookup direction:

- **Influence** (`vkr_reflection_probe_influence`, times the probe's
  `weight`): one inside the box, falling linearly to zero across
  `blend_distance` inward from the four side faces and the top face, and zero
  outside. The bottom face has no band, so a floor lying on it keeps full
  influence. A point within 1 cm outside a face still counts as inside
  (`VKR_REFLECTION_PROBE_WALL_TOLERANCE`). The band lies inside the box, so a
  room's probe does not reach through its walls into the next room.
- **Priority blend** (`vkr_reflection_probe_share`, desktop pipeline): in
  host order each probe takes `influence × (1 − covered)`, where `covered`
  is the sum of the shares before it, and the loop stops once `covered`
  reaches one. The global environment takes `1 − covered`. A room probe
  inside a hall probe owns its room; the hall fills only the room's band and
  the rest of the hall (`packet_environment_terms_with_diffuse_normal` and
  `packet_environment_specular_radiance` in
  [`default.slang`](../../renderer/src/shaders/vulkan/slang/world/default.slang)).
  The tiled pipeline takes one probe per surface by the same order
  ([ADR-087](087-gpu-class-graphics-pipelines.md), decision 8).
- **Lookup direction.** Every environment lookup, global and probe, uses the
  dominant direction of the GGX lobe (Lagarde and de Rousiers, 2014):
  `normalize(N + (R − N) · s (√s + α))` with `s = 1 − α` and `α` the squared
  perceptual roughness (`vkr_specular_dominant_direction`).
- **Box projection** (`vkr_reflection_probe_direction`), authored per probe
  as `box_projection` (default true); false looks up the dominant direction
  itself, as an outdoor probe needs, and does not change the influence. The
  shaded position is clamped to the box, a ray along the dominant direction
  finds where it leaves the box, and the lookup points from the capture
  origin, where the cube was captured, to that point. The result then blends
  toward the unprojected direction by `α`, because a rough lobe spans much of
  the cube. One axis-aligned box serves influence and projection; there is no
  separate projection box or rotation. A baked cube records its capture
  origin (below), and both pipelines' probe record carries it in
  `capture_origin` (`VkrFrameIblProbe.capture_origin`; a probe without a
  record uses its centre), so moving a box keeps the parallax right. Its cube
  still shows the room from the old place: a probe whose centre lies more
  than 1 cm from the origin (`VKR_PROBE_CAPTURE_ORIGIN_TOLERANCE`) is marked
  `capture_moved`, logged once and drawn orange in the editor until Bake
  lighting captures it again (audit B7). `vkr_bakery bake probe --check`
  reports such a cube stale, because the move edits the scene it hashes.

A probe's `intensity_box.x` carries its own intensity, the frame's
pre-exposure P and its source's bake scale, `intensity × P × 2^−B_probe`
([ADR-081](081-physical-night-sky.md); `vkr_vk_upload_packet_tables` in
[`vkr_vulkan_draws.c`](../../renderer/src/vulkan/vkr_vulkan_draws.c),
`vkr_metal_packet_write_upload_environment` in
[`vkr_metal_packet_resources.inc`](../../renderer/src/metal/internal/vkr_metal_packet_resources.inc)).
Its `diffuse_intensity` and `specular_intensity` scale its own terms. The
environment's `intensity`, `diffuse_intensity` and `specular_intensity`
(`ibl_controls`) scale only the global environment's share: a capture
already holds the sun, lamps and sky its room sees. For the same reason
baked sky visibility scales only the global share
([ADR-054](054-baked-diffuse-volumes.md), "Baked sky in environment
reflections"). The global environment remains the unbounded source.
Diffuse uses ADR-038 coefficients and specular uses prefiltered cubemaps.

**Time of day (audit B4).** A baked probe's intensity also carries a
factor `r` for the hour, which the shared runtime multiplies in before
either backend packs the probe (`vkr_standard_scene_runtime.c`), so both
pipelines take it without a shader change. The cube's capture record
([`vkr_probe_capture.h`](../../runtime/src/assets/vkr_probe_capture.h))
holds, for each layer i of the scene's diffuse volume
([ADR-054](054-baked-diffuse-volumes.md)), its baked luminance `E_i` at
the capture origin and the weight `c_i` the volume composed it with
while the faces rendered. With the volume's current weights `w_i`
([ADR-090](090-time-of-day.md)),
`r = clamp(Σ w_i·E_i / Σ c_i·E_i, 1/64, 16)`.
`vkr_scene_update_reflection_probe_light` recomputes `r` when the
volume's composition advances. The clamp is asymmetric: the ratio is one
luminance at one point, so it extrapolates as it strays from one;
darkening by up to 6 stops lets a noon capture fall into dusk, while
brightening amplifies the capture's noise and its stale sky and stops at
4 stops. A probe without a record, without a volume, captured outside the
volume or with a volume whose layer table differs keeps `r = 1`; the last
case logs once. The reflected colour and the sky seen through a window
stay the capture's: a sunset tints the room only through the volume's
diffuse light.
Prepared scene metadata can override exact glTF light-definition ranges at the
cold import boundary; malformed or unmatched overrides fail preparation.

Directional lighting samples CSM. Point and spot lights use a separate
bounded depth atlas when `casts_shadow` is set. glTF imports enable it by
default for finite positive ranges and supported spot angles. Unlimited-range
lights and spots with a 90-degree outer half-angle remain unshadowed, preserving
valid glTF imports without inventing a range or changing the authored cone.
Saved editor overrides can disable shadows; scene-authored JSON lights remain
opt-in.

Local shadows are a persistent cache, phase 1 of the
[local shadow architecture](../proposals/local-shadow-architecture.md) with a
fixed atlas budget. A spot owns one perspective face and a point six, in +X,
-X, +Y, -Y, +Z, -Z order. Each face is a square of one shared 4096-squared
D16 atlas array that every frame in flight samples; a layer is 32 MiB. The
atlas holds at most the preset's layer budget,
`VkrShadowConfig.local_shadow_atlas_layer_budget`: Balanced 8, High 12 and
Ultra 16 layers, the dynamic band and the baked-lamp layer included; zero
allows 32, and `VKR_LOCAL_SHADOW_ATLAS_LAYERS=<1..32>` overrides it for
diagnosis. The band and the baked layer take their layers first and the static
squares keep at least one layer, so a diagnostic budget below that sum is
exceeded. While the faces at their range sizes fit the remaining layers,
every shadow-casting light of the scene table is resident, a face's side
follows only its light's range and the atlas takes as many layers as its
resident faces need. That side is the power of two nearest the range times
64 texels per metre, from 128 to the preset's largest face, so a 7.5 m Bistro
lamp takes 512 squared and the 15 m lamp 1024 squared under High, and camera
motion never resizes a face. Bistro's 72 lamps fit every preset: Balanced's
budget leaves exactly the 7 static layers they need at 512 squared beside the
band. Past the budget
([`vkr_local_shadow_fit_budget`](../../runtime/src/renderer/systems/vkr_local_shadow_system.c))
the atlas keeps every layer it may take, so a change of lights does not
recreate it. Lights farther from the camera than the fade distance plus their
range leave it, and a resident light stays until its distance exceeds 1.15
times that reach. Then the lights of lowest distance importance (below)
shrink one size at a time until half a layer stays free for packing, and a
light resists a shrink below its previous size until a competitor is 1.5
times more important; if every light reaches the smallest size, the least
important leave until the rest fit. Turning the camera therefore resizes
nothing. A light that returns or resizes has invalid
content and draws first within the face budget; if the budget cannot draw it
in that frame, its shadow is hidden until it draws and then fades in. The
ranking takes each light's intensity before the light table's fade
(`VkrLocalShadowCacheInput.unfaded_intensity`, 2026-10-12), so a light
fading out of the table keeps its face size and content while it shows.
Squares pack in descending size by layer
and first free aligned cell; power-of-two squares that fit by area always pack.
A light whose size and layer count are unchanged keeps its squares.

The atlas is D16 on both backends: Metal render pipelines carry no depth
format, and Vulkan draws local faces with D16 variants of the shadow pipelines
and requires D16 depth attachment, sampling, comparison and linear filtering
(ADR-023). Receivers keep their texel-footprint bias. On Bistro, on the Metal
desktop implementation, removed on 2026-10-06 (Release, M1 Pro, 2026-10-03),
it halved the atlas from 768 to 384 MiB (12 layers) with unchanged redraw
counts and local-shadow pass times, and final colour against D32 stayed within
the default snapshot gate in the street overview (1 pixel above 10/255), a
street-level facade view (0.011% of pixels above 2/255) and a bright-spot view
(maximum 3/255). On Vulkan (RX 6700 XT, driver
26.6.3, Release, 2026-10-03) the same comparison at `b7fd519f`, with only the
atlas format and its two pipelines switched back to D32, failed the default gate
in `local_shadow_bistro_vulkan_street_capture`: mean error 0.000199 (limit
0.000392), but 0.65% of pixels above 2/255 (limit 0.1%) and a maximum of 60/255.
Two D16 runs differed in 0.0011% of pixels. The differences lie on foliage
cutouts and thin geometry edges; the captures show no acne or light leaks.
Depth steps grow with the square
of the distance from the light, about 7 cm at 15 m for the 0.05 m near plane,
so longer-range lights would need a larger near plane or more bias.

A face's content is invalid when no successful submission drew it into its
current square with its current projection in the current atlas image, or its
layer was cleared, and stale when a static-world change since it was drawn
reaches the light's range sphere, or the caster publication generation changed.
That generation counts the completed publications a shadow reads: a material's
first publication, an unpublication that no republication follows in the same
frame, a change in its alpha mode, culling, transmission or pipeline, any
publication of a cutout, blended or transmissive material, and a texture or
sampler such a material samples (`vkr_material_system_take_shadow_change`,
`vkr_material_system_shadow_reads_texture`). An opaque material's shadow reads
its geometry alone, so streaming its textures in redraws no shadow; before,
each such republication redrew every local face in range and every cascade. A
geometry publication cannot change a drawn caster either, since a mesh draws
only after its geometry has published and adding or removing a drawn mesh is a
static change. The world payload lists each static change with the world box it
may alter (`VkrWorldPassPayload.static_changes`): a mesh's arrival or departure
its bounding box, a terrain swap the footprints of the tiles that changed. A
change the list does not bound, or content older than the list, reaches every
light. Content that no change reaches takes the new generation. Stale
content keeps showing while it waits to redraw.

A cached face, its static square, holds static casters only, so a moving
caster never makes it stale. Each light carries a six-bit mask of the faces
whose pyramid a dynamic caster's bounding sphere meets
(`vkr_shadow_system.c`). A face in that mask that the camera can see gets a
dynamic square for the frame, in a band of one trailing atlas layer that
exists while the world holds any dynamic caster: `Shadow.Local.Copy.${i}`
copies the static square into it (a same-image layer copy) and
`Shadow.Local.Dynamic.${i}` draws only the dynamic casters there, without a
clear. Culling views carry `STATIC_CASTERS_ONLY` or `DYNAMIC_CASTERS_ONLY`
(`VkrGpuLodView.flags`) and the cull roots the static candidate count, since
the candidate table lists static candidates first. The receiver view of such a
face names the dynamic square, and a companion view after `view_count` names
its static square (`dynamic_source_views`); receivers sample whichever square
their view names, so receiver shaders are unchanged. A dynamic square commits
its own history (`VkrLocalShadowDynamicHistory`): its band cell, the
submission that drew the static square it copied, and the dynamic,
publication and atlas resource generations. While none of them changed, the
light draws nothing this frame and the band layer kept its content, the face
keeps the square in its cell and takes no companion view, copy or render slot
([`vkr_local_shadow_reused_dynamic_faces`](../../runtime/src/renderer/systems/vkr_local_shadow_system.c),
`VkrLocalShadowPassPayload.dynamic_reused_count`). A motionless dynamic
caster therefore costs each face it reaches one copy and one draw, then
nothing while it stays. The dynamic generation
(`VkrWorldPassPayload.dynamic_generation`) advances on any change to a
dynamic caster and on topology changes; a pending publication or an
unavailable scan reuses nothing. When the caster leaves, the face shows its
static square again without a draw. Dynamic faces, drawn or kept, number at
most 16 per frame and each reserves a render slot before the static fill, so
the static budget is the smaller of the face budget and the slots left
whether a square is kept or drawn; a face over the cap shows its static
square, and `draw.local_shadow.dynamic_faces_dropped` counts it. The frame
falls back to the full redraw of every face a caster reaches, with static and
dynamic casters together, when the dynamic scan is unavailable or a
publication is pending (all lights), or when a refractive dynamic caster
reaches a light (that light). Until 2026-10-11 static squares that filled all
32 layers left no band and forced that redraw too.
The preset's face budget bounds the faces drawn per frame: High 30, Balanced 12
and Ultra 60. Complete lights draw in that budget, invalid lights first, then
lights waiting for transmission layers, then stale ones, each by importance. A
light is shadowed once every face is valid; its strength then rises from zero
over 0.25 s, or at once on the first resolve, a budget change and a renderer
camera cut (the TAA rule: more than 10 m or 60 degrees of turn), since the
image has no history to keep. A resident light whose content stays valid never
fades out with time, so its shadow cannot switch off while the camera moves.
A light that stops casting while its shadow shows, such as one the tiled
pipeline's dynamic-light limit stops shadowing (ADR-087), keeps its resident
faces where they remain free after the casting lights are placed and fades
its shadow out over the same 0.25 s without redrawing, or drops it at once on
a camera cut
([`vkr_local_shadow_gather_leaving`](../../runtime/src/renderer/systems/vkr_local_shadow_system.c)).
Receivers instead see that strength times a fade by camera distance, from one
to zero over the last 5 m before `VkrShadowConfig.local_shadow_fade_distance`,
120 m by default: a light past it is not shadowed, so no pixel filters it, yet
it stays resident and its shadow returns continuously as the camera approaches.
Faded lights draw last. The default was 30 m until 2026-10-02. Because
residency does not depend on it, the distance bounds only filtering, and
distant lights cover few pixels and take the single tap. On the Metal desktop
implementation, removed on 2026-10-06 (M1 host, Release,
`local-offscreen-gpu-single`, matched builds), 120 m shadowed all 72 Bistro
lamps instead of 56 in the street view and 62 in
`local_shadow_cache_bistro_metal_indoor_walk`, a Metal desktop case since
removed, with `Shadow.LocalMask`, the GPU pass sum and median frame time
unchanged within 0.05 ms, with and without TAA. In that walk the unshadowed
share of visible local light fell from 0.42% mean and 6.0% at most to zero; the
street and indoor captures stayed within 6 of 255.
`VKR_LOCAL_SHADOW_FADE_DISTANCE` overrides the distance for diagnosis. Shadowed
lights require a finite positive range, and shadowed spot outer half angles
must be below 90 degrees.

Importance orders the fill and the filter. It is luminance times intensity,
before the light table's fade, times range squared over the squared camera
distance, held constant within a
tenth of the range, so the nearest of several overlapping lights wins. Distance
cannot tell a lamp lighting the street from one enclosed in a building, so
importance is instead each light's measured visible contribution once a sample
exists: deferred lighting sums, over the pixels it shades, the light's
unshadowed luminance at the surface after pre-exposure, x, compressed per pixel
to x / (1 + x) by the shared `vkr_local_light_contribution`, with one atomic
per wave per light into a per-frame-slot buffer the frame reads back. The
readback reaches the cache two to three frames later as a
`VkrLocalLightContributionSample` keyed by render id. Deferred lighting
measures only in frames whose index is a multiple of
`VKR_LOCAL_LIGHT_CONTRIBUTION_PERIOD`, four, which the
`light_contribution_enabled` frame flag selects
([`vkr_render_graph_frame.c`](../../renderer/src/vkr_render_graph_frame.c)), so
the newest sample is at most about six frames old. On Vulkan, RX 6700 XT, in
the Bistro street view at 1920x1080 with TAA, mean `Lighting.Deferred` time
fell from 3.68 to 3.54 ms and the final color did not change. On the Metal
desktop implementation, removed on 2026-10-06 (M1 Pro, Release, the indoor
walk, `local-offscreen-perf-audit-gpu`, two children of 660 frames,
non-authoritative), a period of one against four read mean
`Lighting.Deferred.Fullscreen` 2.284 against 2.257 ms with a 0.22 ms spread,
within noise, and identical `lighting.local_shadow.*` values (72 lights, no
fading). Shadowing never changes the measure. A sample is used only when it is
at most eight frames old and comes from no earlier than the last snap;
otherwise distance ranks, as it always does on the tiled pipeline, which
measures no contribution. `VKR_LOCAL_SHADOW_FEEDBACK=0` restores distance
ranking for diagnosis.

A light whose unshadowed contribution at a pixel, the shared
`vkr_local_light_contribution` taken with the shadow normal, is below
`VKR_LOCAL_LIGHT_CONTRIBUTION_CUTOFF` (0.001 of pre-exposed luminance) takes no
mask slot and no deferred shading; `Shadow.LocalMask` and deferred lighting
apply the same test, so their slots stay in step, while forward and
transmission shading keep every light. Because the threshold follows
pre-exposure, it means the same display contribution by day and night. In
matched runs on the Metal desktop implementation, removed on 2026-10-06 (M1
host), it lowered `Lighting.Deferred` by 0.33 ms in the Bistro street view,
with and without TAA, and by 0.22 ms in the indoor walk, partly because fewer
pixels overflow their eight slots into inline filtering; the GPU pass sum fell
by 0.31 to 0.38 ms. Lit surfaces changed by more than 2 of 255 on at most 41
pixels of the street and indoor captures (cloud pixels moved by up to 40
through the reordered sky evaluation under fast math). Skipping such lights
saved nothing in the mask, whose single-tap cost comes from lights that matter.

Each shadowed light adds its filtering to every pixel in its range. The two
most important shadowed lights take the nine-tap filter and contact shadows;
the others take one hardware-filtered comparison tap and no contact shadows,
flagged by `shadow_params.z`. A light that had the full filter keeps a 15%
incumbent preference for it, so its filter does not flip while scores cross.
Before the cache, on the Metal desktop implementation, removed on 2026-10-06
(M1 development host, 1280x720, Bistro street view), five full-filter lights
cost 3.0 ms more `Shadow.LocalMask` time and 3.8 ms more frame time than three,
and four cost 1.5 ms and 2.2 ms more; five lights with two reduced cost 0.6 ms
more mask time and 0.95 ms more frame time than three, because forward and
transmission shading also filter the extra lights. On 2026-10-03 the count fell
from three to two
([`vkr_local_shadow_system.c`](../../runtime/src/renderer/systems/vkr_local_shadow_system.c)).
The tiled pipeline sets `VkrShadowConfig.local_shadow_full_filter_all` every
frame, so each of its at most four shadowed lights takes the full filter, its
four-comparison tent (ADR-087); the desktop pipeline keeps two.
On Vulkan, RX 6700 XT, in the Bistro street view at 1920x1080 with TAA and the
High preset (`local_shadow_bistro_vulkan_street` at that size, one child of 240
frames under `local-offscreen-gpu-single`), `Shadow.LocalMask` fell from 2.84
to 2.66 ms and the GPU frame from 11.15 to 10.94 ms. The final color changed in
0.14% of pixels by more than 8 of 255, at the shadow edges of the third light,
and a repeated capture of one build changed none. On the Metal desktop
implementation (M1 Pro, Release, 1280x720, TAA,
`local-offscreen-perf-audit-gpu`, two children each, non-authoritative), three
against two lights read `Shadow.LocalMask` 3.094 against 2.805 ms and median
frame 14.71 against 14.33 ms in `local_shadow_taps_bistro_metal_street_taa`,
and 1.314 against 1.249 ms and 11.70 against 11.50 ms in
`local_shadow_taps_bistro_metal_indoor_walk_taa`, Metal desktop cases since
removed. The street capture changed in 0.10% of pixels by more than 8 of 255
(at most 33), along one pillar edge and not visible side by side, against a
repeat floor of 5; the indoor capture changed at most 4.

Under temporal reconstruction `Shadow.LocalMask` filters the full-filter
lights with the first four taps of the progressive Poisson table instead of
nine, rotated per pixel by interleaved gradient noise that advances with the
contact-shadow noise index, and TAA integrates the rotations
([`local_shadow.slangh`](../../renderer/src/shaders/shared/local_shadow.slangh)).
Without temporal reconstruction the mask keeps the fixed nine taps, and
lighting's inline fallback, the clearcoat query, forward and transmission
shading always do. On the Metal desktop implementation, removed on 2026-10-06
(M1 host, Release, the street view with TAA at 1280x720, profile
`local-offscreen-gpu-single`, timestamps on), the mask fell from 6.44 ms to
4.61 ms and the frame from 20.2 ms to 17.6 ms median; in the indoor walk with
TAA (0.75 scale) the mask fell from 2.03 ms to 1.65 ms. The street capture
after TAA changed at most 9 of 255, with 0.26% of pixels changing by more than
2, against a repeat floor of 3; the indoor capture changed at most 1. Without
TAA the street capture was byte-identical. Native Vulkan, the only
implementation since 2026-10-06, has not run this comparison.

Forward and transmission shading filter full-filter lights inline with the
first four progressive taps, unrotated (`VKR_LOCAL_SHADOW_INLINE_TAP_COUNT`).
They shade few pixels, so rotation would add plumbing without visible gain: on
the Metal desktop implementation (M1 host) transmission shading fell from 1.94
to 1.53 ms in the street view (1.99 to 1.56 ms with TAA, 0.94 to 0.86 ms
indoors) with at most 0.04% of pixels changing by more than 2 of 255. A single
tap saved 0.23 ms more but changed 0.24%. Lighting's inline fallback and the
clearcoat query keep nine taps.

Application preparation owns the fixed frame-local view payload. Its views are
the faces of the shadowed lights, rebuilt each frame; `light_first_view` names
a light's contiguous faces, and view indices need not persist because history
belongs to the cache light, keyed by render id. Each view carries its atlas
square and layer, and frame validation rejects squares that are unaligned, out
of range or overlapping. GPU culling tables size storage from the current
camera, directional, opaque local and transmitting local view count and retain
grown capacity. Opaque local faces exclude refractive casters whenever the
scene has them; transmitting views select them; directional caster
classification is unchanged.

The atlas and the transmission arrays are single retained graph images, not
one per target image. A frame that redraws a face an earlier frame in flight
still samples is ordered behind it by the graph's retained state: the first
use of an image in a frame waits on the previous submission's terminal
access, through queue-scoped barriers on Metal and pipeline barriers on the
one Vulkan queue. The renderer supplies the atlas generation, layer count and
valid layers before the resolve. Face passes load the atlas and clear only
their own square, so `Shadow.Local.Clear.${i}` clears each layer without
retained content first; faces of a cleared layer that this frame does not draw
lose their content. An image created by a frame has generation zero in that
frame's history and is adopted when the next frame sees its generation. Shadow
strength, filter and transmission layer are receiver-side and never force a
redraw. The dynamic scan is bounded by `reuse_dynamic_scan_budget`; exceeding
it marks every face stale. Pending history is committed only after submission
succeeds and is discarded when the frame is cancelled or fails. Local shadows
do not use directional fit retention or SDSM.

The payload separates the views receivers sample from the faces drawn this
submission. Each drawn face takes a render slot, at most 64 per frame; slot i
owns opaque culling view i, transmission culling view i and the repeated
`Shadow.Local.*` passes of index i, and names the view it draws. The graph
resolves a slot's attachment to that view's atlas layer and transmission layer
through the `${local_shadow_render_atlas_layer}` and
`${local_shadow_render_view}` slice tokens. A reused face has no culling view,
so culling and indirect-command work scale with drawn faces only. On Metal, a
per-view command reset for reused faces had cost about 0.8 ms per frame for
three cached point lights in the Bistro street view before reused faces had
culling views removed.

Local transmitting shadows retain two ordered surface crossings at 512², capped
by the configured opaque-map extent, in transmission arrays of one layer per
face of the face budget. The most important lights, a light that holds layers
weighted by four times its importance, hold a layer for every face; refractive
casters do not shadow the other lights' faces, whose opaque maps exclude them.
`VkrLocalShadowView.shadow_params.y` is the face's layer plus one, or zero. A
light takes layers only in a frame that draws it, the lowest free first, so the
layers below the first never-drawn one are the ones receivers read. Each array
keeps at most the texels of 32 faces at 512², so a larger face budget halves
the crossing size instead of growing the pool: Ultra's 60 layers take 256². Each crossing stores D32 depth and
RGBA16F cumulative RGB transmission. A third depth-only crossing blocks
receivers beyond capacity. Receivers before a crossing do not inherit its
attenuation. Each PCF tap selects its depth-gated prefix, multiplies it by
opaque visibility, then contributes to the average. Point taps retain cross-face
reprojection; volumetric injection uses the same RGB visibility with its
existing single tap.

The crossing coefficient uses authored transmission, base color, metallic,
layered GGX/sheen/clearcoat reflection loss and volume absorption along the light
ray. Material textures and cutout coverage are evaluated in the shadow raster;
a zero-transmission texel blocks light. Authored thickness supplies absorption
length after directional instance scaling. Rays remain straight and do not
produce refracted caustics. Thin-sheet diffuse transmission remains an opaque
shadow caster. This local-light model does not change directional shadows.

The five transmission arrays share the atlas's single graph owner, retirement
and submission lifecycle. A light with layers draws its opaque faces and all
its crossings together, except that a light whose opaque faces are valid and
not stale draws its crossings alone: its slots are set in
`VkrLocalShadowPassPayload.retained_opaque_mask`, and their opaque passes
neither clear nor draw their squares. Its transmission is valid only with
matching generations for all five arrays and valid contents in each of its
layers; an incomplete or replaced prefix pool redraws its crossings, and the
light keeps its opaque shadow meanwhile.

While the camera moves, the lights holding layers change. Orbiting Bistro at 18
m on the Metal desktop implementation, removed on 2026-10-06 (M1 Pro, GPU pass
timing), about 24 faces a second took layers, each light costing 4.6 ms of GPU,
2.9 ms of it for opaque faces that were still valid. The fourfold preference
took a quarter of those changes (a twentyfold one still left 35 in the orbit:
the lights that matter change), and drawing crossings alone removed the opaque
cost. After the first two seconds, frames above 16 ms of GPU fell from 17–19 to
none and the slowest from 23.7 to 16.0 ms; untimed interleaved runs of both
builds kept their 11.3 ms mean while their slowest frame fell from 24–25 to 16
ms. A camera jump still fills the cache 30 faces a frame. Native Vulkan, the
only implementation since 2026-10-06, has not run this change. Cancelled work
never promotes history. Scenes with no refractive candidates allocate no
transmission pool or views.

A scene reflection probe may name one saved source cubemap with
`reflection_probes[].cubemap.path`. The direct path and legacy
`base_path`/`extension` face-set route are mutually exclusive; a direct path
cannot carry either legacy field. The asynchronous path owns a prepared direct
source until finalization; the synchronous path owns equivalent local prepared
storage. Both require a cube texture, finalize it on the owning path, and hold
a texture reference through scene ownership. Async payload destruction and
post-finalize cleanup release prepared-load storage. A failed direct source
disables that probe instead of aliasing the environment.

The runtime uses a direct source cubemap once: it creates one writable prefilter
cubemap and asks the retained IBL publisher to derive both the probe's SH
coefficients and prefilter. Source mip count comes from the finalized texture,
so the prefilter's filtered importance sampling reads the source mips the
baker wrote; a one-mip source, such as a cube baked before 2026-10-12, falls
back to mip 0 and aliases on rough surfaces. The runtime does not render six
scene views for a saved probe. A probe with no cubemap continues to alias the
ready scene environment.

A probe without an authored `sh_deringing` takes a window of 1.0
([ADR-038](038-sh-l2-diffuse-irradiance.md)).

[`vkr_bakery bake probe`](../../tools/bakery/vkr_bakery_bake.c) owns offline
six-view capture in one harness session. A bake records
the source scene manifest, per-face capture and report digests, output digest,
and one stable native provenance record in `<output>.bake.json`. It captures the
six KTX cube faces in `+X, -X, +Y, -Y, +Z, -Z` order with local probes disabled,
then the packer vertically flips each top-left capture exactly once. The
capture's visible sky leaves out the sun and moon discs, their glow and the
stars, as the global IBL source does
([ADR-058](058-revision-baked-sky-atmosphere.md)), so a probe does not
reflect the sun a second time over its direct light: the harness child sets
`VkrFrameGlobals.sky_light_sources_hidden`
([`vkr_frame_input.h`](../../renderer/src/vkr_frame_input.h)) for a case
with cubemap camera modes, and `vkr_renderer_prepare_frame_data` zeroes the
sky record's `solar.w`, `lunar.w`, `mie_extinction.w` and `star_axis.w`.
`--size` is a power of two from 1 to 256, the runtime prefilter size
(`VKR_IBL_PREFILTER_SIZE`); a larger capture would only alias the top level.
The emitted `.vkt` is an uncompressed KTX2 cube with six RGBA16F faces, one
array layer and the full mip chain, `log2(size) + 1` levels
([`vkr_hdr_cube_packer.cpp`](../../tools/vkr_hdr_cube_packer.cpp)): each
level is a 2×2 box filter of the level above, averaged in fp32 and rounded
to the nearest even binary16. It has no orientation repair.

The capture holds the static world only (audit B5). For a case with
cubemap camera modes the harness child sets the runtime's
`static_capture`, and `vkr_scene_build_world_draws`
([`vkr_scene_frame.c`](../../runtime/src/renderer/systems/vkr_scene_frame.c))
then leaves out meshes and instances of DYNAMIC mobility (movers, physics
bodies, skinned and animated models) and every alpha-blended draw;
anything that moves would stay frozen in the probe. Planar reflectors'
draws carry the camera's hidden bit there too, so a plane's own probe
never sees the plane (ADR-117). Opaque, cutout and
transmissive static surfaces stay, and so does fog, which lies between
the probe and what it reflects. Faces render `--supersample` times
wider than `--size` (1, 2 or 4, default 4, at most 1024 texels), and the
packer box-filters each face down before it builds the mip chain, so a
mirror-like surface reflects anti-aliased edges.

The bake also writes the probe's capture record (time of day, above) into
the cube as the KTX2 key/value entry `vkr.probe_capture`, so the cube
carries it through project lowering and packaging and the scene loader
reads it with the cube's first kilobytes. The harness child records the
diffuse volume's layers and composed weights at the first face and checks
that every later face saw the same composition; it publishes them as the
run's `capture.light_layers` artifact. The bake decodes the one diffuse
volume the capture's scene manifest names, checks that its layer table
matches, and takes each layer's luminance at the capture origin with
`vkr_diffuse_volume_layer_luminance`
([`vkr_diffuse_volume.c`](../../runtime/src/assets/vkr_diffuse_volume.c)):
the constant term of each layer's L1 SH, with a lamp group's direct band
added, interpolated trilinearly over the valid probes of the brick cell,
without relocation or visibility. The sidecar records the supersampling
and a summary of the record under `capture_record`, and its probe recipe
version is 2, so `--check` reports a cube baked before these changes as
stale.

A scene may name the bake's own output as its probe's cubemap, so a re-bake
replaces the cube the scene uses. Captures turn local probes off, so that
cube never feeds the capture. The scene-manifest check
(`vkr_bake_scene_manifest`) skips the output: it neither compares the file
with its recorded digest nor counts it as a source the output would
overwrite, and the recorded digest still enters the aggregate. Output
protection and `--check` freshness therefore work for such a scene.

A project bake (`vkr_project_perform_bakes` in
[`vkr_project_bake.c`](../../tools/bakery/project/vkr_project_bake.c)) bakes
the diffuse volume, then lightmaps, then probes. The earlier bakes point the
scene at their new artifacts, so probes capture this bake's light.

Example bake and freshness check:

```sh
./build_release/tools/bakery/vkr_bakery bake probe \
  --scene assets/scenes/example.json \
  --position 0 1 0 \
  --output assets/probes/example.vkt

./build_release/tools/bakery/vkr_bakery bake probe \
  --output assets/probes/example.vkt --check
```

Bistro's café probe (box centre (-7.5, 2.2, 9.0), half extents
(5.5, 2.5, 8.0), band 0.75 m) reads
`assets/textures/bistro_cafe_probe.vkt`, a 256² cube of nine levels, about
4.2 MB. The cube and its `.vkt.bake.json` are tracked as an exception to the
`.vkt` rule in `.gitignore`: an uncompressed RGBA16F cube is the same on
every host. It is captured from
`assets/scenes/fixtures/bistro_sparse_volume_local.scene.json`, which names
Bistro's untracked diffuse volume, so the room in the cube holds baked bounce
light ([ADR-054](054-baked-diffuse-volumes.md)); the tracked
`bistro.scene.json` names no volume. Re-bake it with:

```sh
./build_release/tools/bakery/vkr_bakery bake probe \
  --scene assets/scenes/fixtures/bistro_sparse_volume_local.scene.json \
  --output assets/textures/bistro_cafe_probe.vkt \
  --position -7.5 2.2 9.0 --size 256 \
  --harness build_release/tools/vkr_harness.exe
```

`--harness` is needed on Windows only: the default,
`build_release/tools/vkr_harness`, has no `.exe`.

`bistro.scene.json` and the 28 fixtures that keep Bistro's sky point their
probe at this cube with `specular_intensity` 1. The 16 fixtures with another
atmosphere or environment, such as night or another sun, still have no
probe: the time-of-day ratio follows the baked light's brightness, not
another sky's colour, and none of them was checked with the probe.

Desktop receivers use nine comparison-PCF Poisson taps with a 1.5-texel radius,
in texels of the face that owns each tap. Normal offset is two local shadow
texels and receiver bias is one texel, converted using the perspective
footprint at receiver depth. Point taps reconstruct rays and reproject into
adjacent faces. A tap that stays inside the receiver's face derives its
reference depth from the face projection instead of reprojecting. Each tap maps
its face UV into the face's atlas square, clamped half a face texel inside it
so bilinear comparison never reads a neighbouring face. Both deferred and
transparent lighting consume the same local visibility semantics.

Point and spot lights author `source_radius`, the emitter radius in metres,
finite in `[0, 1]` (`VKR_POINT_LIGHT_SOURCE_RADIUS_MAX`); zero, the default and
every glTF import, keeps the fixed 1.5-texel disk exactly. In `Shadow.LocalMask`
only, a full-filter light with a nonzero radius hardens at contact: blocker
search taps read raw atlas depths at the first progressive Poisson taps, eight
unrotated without temporal reconstruction and the first four with the filter's
per-pixel rotation under it, clamped inside the receiver's face. Point-light
search taps compare against the receiver's radial depth like the filter taps.
By similar triangles the search radius is `r (d_r - n) / (2 t n d_r)` face UV
and the filter radius `r (d_r - d_b) / (2 t d_b d_r)`, with `d_r`, `d_b` and
`n` the receiver, average blocker and near forward distances along the face
axis and `t` its tan(half FOV); stored depths invert through the face clip
terms. Both radii are clamped to `[1.5, 8]` face texels. A blocker's projected
offset equals the filter radius it produces, so a search capped at the filter
cap finds every blocker that can widen the filter. An empty sparse search
keeps 1.5 texels, since it does not prove the receiver lit. Tap counts,
transmission lookups, reprojection across faces and contact shadows are
unchanged; single-tap lights, inline forward and transmission filtering and
froxel injection keep the fixed radius. The CPU writes `r / (2t)` per face into
`VkrLocalShadowView.shadow_params.w`, zero for single-tap lights or when the
**Soft shadows** setting (`VkrShadowConfig.local_shadow_soft`) is off. The value
is receiver-side: changing it moves the temporal scene signature but never
redraws a face. Bistro's 72 imported lamps carry 0.05 m through
`assets/scenes/bistro.scene.json.editor.json`.

Contact-hardening evidence, on the Metal desktop implementation, removed on
2026-10-06 (Release, M1 Pro, 2026-10-05, dirty tree, non-authoritative): with
every radius zero, the street local-shadow capture is byte-identical to the
`24636515` binary. With the Bistro radii, 1.5% of its pixels change by more
than 2 of 255, along lantern shadow edges, and a TAA capture of the same view
showed no added noise. `bistro_native_perf_audit_steady`, a Metal desktop case
since removed, under `local-offscreen-perf-audit-gpu` (five children of 300
frames, TAA off, same binary, radii on against the override file removed)
measured `Shadow.LocalMask` at 4.29 ms against 3.77 ms p50 (spread 0.01 ms),
frame 17.18 ms against 16.69 ms, and unchanged `Lighting.Deferred`. Native
Vulkan, the only implementation since 2026-10-06, has not run contact
hardening.

Deferred lighting does not filter local shadow maps itself. The
`Shadow.LocalMask` compute pass runs before it while a shadowed light's range
sphere, widened by 0.05 m, meets the frustum of the jittered camera
projection that drew the visibility buffer, or while baked lamps' moving
casters are shown (`local_shadow_mask_active`,
[`vkr_render_graph_frame.c`](../../renderer/src/vkr_render_graph_frame.c),
2026-10-11); a light without a finite range always counts. Every visible
pixel lies in that frustum, so a skipped pass leaves no pixel that needs it,
and the four deferred-lighting passes read the mask under the same condition.
The frame's mask of those lights (`VkrVulkanLocalShadowMaskRoot.mask_lights`)
limits the pass's light traversal: a pixel in no such light's grid cell
returns before its normal, noise and material loads, and the baked-lamp
kernels skip only the material load. For every other opaque pixel the pass
stores the filtered RGB visibility, with strength
applied, of the k-th shadowed light in range of the pixel that can light it, in
light traversal order, in layer k of an eight-layer full-resolution RGBA8
array. A light can light the pixel when it lies in front of the shadow normal,
which back-lit diffuse transmission flips toward it
(`vkr_local_shadow_light_faces`); otherwise every lobe that uses the base
visibility is zero, so no receiver looks it up and the mask gives it no slot. A
coat whose normal differs keeps its own inline lookup. Alpha tags the layer
with the light index plus one over 255. The pass repeats deferred lighting's
world-position reconstruction, light traversal, range and cone tests, back-lit
normal choice and facing test, so lighting counts the same lights in the same
order and reads layer k for its k-th shadowed light. A light past the eighth,
or whose layer tag names another light because a boundary test differed between
the passes, is filtered inline by lighting without contact shadows. Eight
layers therefore bound the shadowed lights overlapping a pixel, not the
shadowed lights in the frame. The clearcoat's second visibility query for a
differing coat normal, forward shading, and transmission shading still filter
inline. On the Metal desktop implementation, removed on 2026-10-06 (M1 host,
Bistro street view), the mask pass cost about 2.3 ms and lowered
`Lighting.Deferred` from 6.7 ms to 3.3 ms, a net 1.1 ms of GPU time. Output
matches inline filtering within 8-bit quantization of visibility. A
half-resolution RGBA16F mask with depth- and normal-aware upsampling was
measured and rejected: at equal quality it was slower than the full-resolution
mask, and without an inline fallback it lost sharp local shadow detail on thin
geometry near lights.

The mask pass also applies contact shadows to each shadowed light. From a start
offset along the shadow normal, it marches eight depth-buffer samples over at
most 0.25 m toward the light, and never more than half the distance to it. A
sample is occluded when the surface on its camera ray lies in front of it by
more than 0.4% of the camera distance but less than 3 cm plus 0.5% of it. The
thin limit keeps rays that pass behind lamp frames and other nearby geometry
unoccluded. Occlusion in the far half of the ray fades out. The result is
blended by the light's strength and multiplies its filtered visibility, so
contacts the map's filter radius, normal offset and bias remove, such as vines
on a wall, keep a shadow. The march start follows an R2 sequence over the pixel
that advances with the frame index when temporal reconstruction is enabled, so
TAA integrates the step pattern. Contact shadows are screen-space: occluders
off screen or hidden behind nearer surfaces cast none, and forward and
transmission shading do not apply them. On the Metal desktop implementation (M1
host, Bistro street view) they added about 0.85 ms to the mask pass. With every
lamp shadowed (56 lights in that view, 2026-10-02, TAA off) removing the march
saved 1.49 ms of the 6.34 ms mask, but four steps instead of eight saved only
0.22 ms, and one march per pixel after the light loop, toward the full-filter
light with the largest shadowed contribution, saved 0.04 ms while dropping the
other lights' contacts, so it was rejected. Replacing each step's world
reconstruction with the ratio of view depths along the sample's camera ray, one
dot product instead of a 4x4 transform, left the mask at 6.44 ms and was not
kept either. The cost follows the march's presence in the kernel rather than
its arithmetic.

On the Metal desktop implementation, removed on 2026-10-06, a Metal System
Trace with GPU counters of the street view, with the mask in its own encoder,
explained why: the mask ran at 18.5% compute occupancy with no limiter above
40% (ALU 40%, texture sampling 13%, last-level cache 13%), so it was
latency-bound, and the compiler allowed it only 576 threads per threadgroup,
640 without the march. With threadgroup-size hints the march still cost 1.1 ms
(5.78 against 4.67 ms without it). A separate `Shadow.LocalContact` pass that
rescanned the mask slots and marched for the full-filter lights was built and
measured there: the mask fell to 4.64 ms but the contact pass took 1.36 ms,
and 1.18 ms with the view-depth march and its depth reads issued before any
test, so mask and contact together stayed above the single kernel (5.84
against 5.74 ms). It was not kept; the march costs about 1.1 ms wherever it
runs, and only fewer marches or steps, a quality decision, would lower it.

Contact shadows therefore belong to the Ultra preset only
(`VkrShadowConfig.local_shadow_contact`, carried to the backend as
`VkrLocalShadowPassPayload.contact_shadows`). Vulkan builds the mask twice,
`vk_local_shadow_mask` and `vk_local_shadow_mask_contact`, so the default
kernel carries none of the march's registers. In the application they follow
the "Contact shadows" graphics setting, which the Epic preset enables; harness
cases enable them through the `ultra` shadow preset. On the Metal desktop
implementation (M1 host, matched builds, default preset) the mask fell from
5.68 to 4.61 ms in the street view without TAA and from 4.18 to 3.15 ms with
it, the frame median from 16.3 to 15.3 ms with TAA, and indoors from 1.77 to
1.60 ms; an Ultra capture was unchanged.

Probe bounds/ranges are not geometry visibility. The removed hard influence-AABB
experiment is not an occlusion mechanism. GTAO attenuates local indirect diffuse
only and does not establish arbitrary wall or furniture occlusion.

## Consequences

Lighting is bounded and independent of draw partitioning. Unshadowed lights and probe bounds can still leak illumination through geometry.
The atlas takes at most the preset's layer budget, 256, 384 or 512 MiB, one
image for all frames in flight; within it every resident face costs atlas
memory whether or not it is on screen. Bistro's 71 lamps of 7.5 m take 426
faces at 512 squared and its 15 m lamp six at 1024 squared under High, eight
layers or 256 MiB; the per-image atlas it replaces took 64 MiB per target
image. These are storage figures from the layout, not a measured allocation.
A scene past the budget gives distant and less important lights smaller
faces or no shadow, and a camera cut can resize many lights at once, each
hidden until the face budget draws it. A face redraws whenever its content is
invalid or stale, within the face budget per frame; after the fill, cost
scales with moved lights and with the faces that changing dynamic casters
reach, each a copy and a dynamic-caster draw. The dynamic band takes one of
the budget's layers while a dynamic caster exists. A scene load draws its faces over
several frames: 432 Bistro faces take 15 frames under High. The shadow mask
adds 32 bytes per pixel while local shadows are active, about 28 MiB per
physical image at 1280x720. With refractive casters, the single transmission
pool adds up to 224 MiB (32 layers at 512² or 64 at 256²), excluding
allocation alignment and culling buffers: High's 30 layers take 210 MiB and
Ultra's 60 take 105 MiB. Each light drawn with transmission layers adds two
material/depth passes and one overflow-depth pass per face. These are storage
and pass budgets, not a timing claim.

Saved probes consume a source cubemap plus a runtime prefilter cubemap and SH
slot. Their offline artifact is valid only with its matching sidecar provenance;
`--check` verifies its output digest, scene-manifest source digests, face list,
and recorded provenance consistency. The saved source must remain a finite,
uncompressed RGBA16F KTX2 cube with the baker's face order and full mip
chain; a 256² cube takes about 4.2 MB.

Probes blend by box volume, not by authored priority, and the band reaches
inward only, so a probe box must enclose the floor area it serves plus its
band width. Probe light follows the hour in brightness only: a capture holds
the World's sky and colours at bake time, and a probe outside the diffuse
volume does not follow it at all (audit B4 in
[Short-history rendering](../proposals/short-history-rendering.md)). A
probe moved off its capture origin keeps a correct projection but
reflects the room as seen from the old place until it is baked again. A probe capture renders 16 times
the texels of its cube and leaves movers and alpha-blended surfaces out.

## Alternatives considered

A first-N global list makes visibility depend on source order; until
2026-10-11 the table kept the 128 lowest render ids, such a list for scenes
past 128 lights. A Balanced atlas budget of 6 layers would shrink Bistro's
lamps, which need 7 static layers beside the band. Per-draw probe
selection fails on large meshes. Until 2026-10-12 the desktop pipeline
normalized overlapping probes, so a room probe inside a hall probe ended
half and half, and its blend band lay outside the box, crossing walls into
the next room; a point in that outer band also lost box projection, which
left a seam where a floor ran through a doorway. Applying the environment's
gains and baked sky visibility to probes double-counted what their captures
hold. Hard light influence boxes produced discontinuous
slabs without solving diffuse occlusion. Excluding glass from depth-only shadows
would restore light but discard tint and attenuation. Four transmitting crossings
would add 624 MiB across three full-pool target images and five passes per
refreshed face; the accepted budget is two crossings and an opaque overflow
boundary to prevent leaks.

## Revisit when

The remaining phases of the
[local shadow architecture](../proposals/local-shadow-architecture.md) land:
mask filtering bounded by a distance fade, measured cache format and face size,
residency that follows world streaming cells with prefetch (the fixed layer
budget per preset landed on 2026-10-11). A scene past 128 lights or past the
atlas budget shows popping lights or face-size churn. Scene scale exceeds
these capacities, stored probe source requirements change,
or local-light transport requires more crossings, refracted caustics, or a
different memory or raster budget. A scene needs more than 16 probes, an
authored probe priority, probes whose sky or colour follows the hour
beyond one brightness ratio, or a projection box apart from the
influence box (audits B4 and B7).

## Implementation

[`scene_loader.c`](../../runtime/src/renderer/resources/loaders/scene_loader.c)
parses and owns direct source loading. [`vkr_world_resources.c`](../../runtime/src/renderer/systems/vkr_world_resources.c)
requests the one-time probe bake. [`vkr_bakery_bake.c`](../../tools/bakery/vkr_bakery_bake.c)
and [`vkr_hdr_cube_packer.cpp`](../../tools/vkr_hdr_cube_packer.cpp) define the
offline artifact and provenance contract. [`vkr_shadow_system.c`](../../runtime/src/renderer/systems/vkr_shadow_system.c),
[`vkr_local_shadow_system.c`](../../runtime/src/renderer/systems/vkr_local_shadow_system.c),
[`vkr_standard_scene_runtime.c`](../../runtime/src/application/vkr_standard_scene_runtime.c),
and the Metal/Vulkan retained graph providers implement local-shadow reuse,
the atlas budget and submit-only promotion.
[`vkr_lighting_system.c`](../../runtime/src/renderer/systems/vkr_lighting_system.c)
selects the light table, and
[`vkr_render_graph_frame.c`](../../renderer/src/vkr_render_graph_frame.c)
gates `Shadow.LocalMask`.
[`reflection_probe_kernel.slangh`](../../renderer/src/shaders/shared/reflection_probe_kernel.slangh)
defines probe influence, priority share and lookup direction for
[`default.slang`](../../renderer/src/shaders/vulkan/slang/world/default.slang)
and, through
[`library.recipe.json`](../../renderer/src/shaders/metal/library.recipe.json),
for [`lighting.metalh`](../../renderer/src/shaders/metal/msl/world/lighting.metalh),
[`tiled.metal`](../../renderer/src/shaders/metal/msl/world/tiled.metal) and
[`gpu_draws.metal`](../../renderer/src/shaders/metal/msl/world/gpu_draws.metal).
`vkr_standard_scene_runtime_order_probes` orders the frame's probes.
[`vkr_probe_capture.c`](../../runtime/src/assets/vkr_probe_capture.c)
defines the capture record, its KTX2 lookup and the time-of-day ratio;
`vkr_scene_update_reflection_probe_light` in
[`vkr_scene_system.c`](../../runtime/src/renderer/systems/vkr_scene_system.c)
applies it and marks moved probes.

## Evidence

Audits B4, B5 and B7, Vulkan, RX 6700 XT, Windows, 2026-10-12, local and
non-authoritative:
- Debug `build_test.bat` passed every CPU suite, including
  `test_probe_capture_follows_volume_light`: on a one-brick volume the
  sampler reads each layer's constant term, adds the lamp group's direct
  band, interpolates and drops invalid probes; the ratio and both clamp
  bounds match hand-computed values; a record written into a cube by
  libktx, beside another key, reads back, and a cut record is refused.
  Release `build_release.bat` passed.
- The café probe was baked again with the release bakery
  (`vkr_bakery bake probe --scene assets/scenes/bistro.scene.json --output
  assets/textures/bistro_cafe_probe.vkt --position -7.5 2.2 9.0 --size 256
  --harness build_release/tools/vkr_harness.exe`): 1024² faces, cube
  `sha256:bdeee249…50d3`, capture report `sha256:cba6d61d…56c1`; `--check`
  reports `current`, and reported `stale` for the cube of the previous
  recipe. A first attempt failed because the harness child's report had
  no room for the light-layers artifact; the child now sizes one more.
- An independent Python reader of the KTX2 key/value data found the
  record: origin (-7.5, 2.2, 9.0), nine layers. At the café's capture point
  the default lamp group gives 98.6 % of the baked light, so a single sun
  key changes the ratio by −1.1 % to +5.1 %; with the lamps off the
  unclamped ratio is 0.0141 and the clamp holds it at 1/64.
- Debug harness snapshots of `local_shadow_bistro_indoor_capture`: with
  the probe as baked, no probe warning (record read, layer table matched,
  not moved); with a scratch copy of the scene whose probe centre was moved
  1 m, the runtime logged "reflection probe 0 is 1.00 m from where its
  cubemap was captured".
- The bake ran on the working tree's shaders, which held another session's
  uncommitted desktop changes to `deferred.slang` and `default.slang`
  (horizon normal and specular occlusion, audits R8 and R11).
- Not run: Metal (no Mac available), any two-hour harness comparison
  (Bistro has no World time of day), timing.

Probe audit fixes, Vulkan, RX 6700 XT, Windows, 2026-10-12, local and
non-authoritative:
- Debug `build_test.bat` passed every CPU suite, including
  `test_vulkan_shader_abi_reflection`.
- The café probe bake wrote a cube of nine levels.
- Release `vkr_harness snapshot` with `local-offscreen.json`, TAA off, of the
  street camera of `decals_bistro_street_baseline_capture` (1920×1080) and
  the indoor camera of `local_shadow_bistro_indoor_capture` (1280×720),
  against the step-1 build (SSR, SSGI and FSR 3.1 removed, before these
  fixes):
  - Before Bistro took its diffuse volume: street depth and G-buffers
    byte-identical, final colour different by more than 2/255 on 1.7 % of
    pixels, from the dominant-direction lookup. Indoors the café turned grey
    and blue: the old placeholder probe had been a flat warm ambient about 20
    to 60 times brighter than the captured room and gave no specular, and
    without baked light the café's only ambient was the probe's capture.
  - With the volume (final): street depth and G-buffers byte-identical again;
    final colour differs on 96 % of pixels by more than 2/255 and on 37 % by
    more than 16, the volume's bounce replacing the sky's SH. Indoors the café
    is warm again and its bar top and glassware reflect the room; 91 % of
    pixels differ by more than 16/255 from the placeholder render. A small
    green patch of bounce on the street's cobbles comes from the volume.
- `vkr_bakery bake probe --check --output assets/textures/bistro_cafe_probe.vkt`
  reports `current` for the café probe baked from a scene that names the probe
  it replaces. On 2026-10-13 `bistro.scene.json` stopped naming the untracked
  volume and the cube was re-baked from
  `bistro_sparse_volume_local.scene.json` (sha256 `9ec73770…`, `--check`
  `current`), then once more with the B4 record and B5 capture: 1024² faces,
  cube sha256 `d2b6b671…`, `--check` `current`. Its factor across the eight
  sun keys with the lamps as captured is 0.989 to 1.051, and 1/64 (clamped
  from 0.0141) with the lamps off. A café snapshot with it changes
  `final_color` on 6.1 % of pixels by more than 2/255 (mean 0.78/255).
- Café specular on both pipelines (2026-10-13, desktop capture at the indoor
  camera against the Mac's tiled capture): surfaces that turned grey or silver
  once the probe's specular gain went from 0 to 1 are authored as metal or as
  near-black gloss: the pendant shade and floor strip have F0 0.47 to 0.60 at
  roughness 0.30 with zero albedo, the bar panelling F0 0.19 with albedo 0.016,
  and the stools and chairs albedo at most 0.024. Turning the probe's specular
  off restores the old dark look; turning the environment's off does not. Both
  pipelines show the same result, so it is the materials' response to room
  reflections, not a pipeline defect.
- Not run: Metal (no Mac available), so the tiled pipeline's side was
  neither compiled nor run, and no timing.

Static and dynamic squares, Vulkan Release, RX 6700 XT, 2026-10-09, local and
non-authoritative (`local-offscreen-gpu-repeated`, 5 repetitions of 300
frames): on the Bistro street view without a moving caster the GPU frame is
5.749 ms before and 5.755 ms after, `Shadow.LocalMask` 1.816 ms both, and the
band never appears. With the mannequin walking about 3 m from a lamp
(`local_shadow_bistro_vulkan_street_moving`), the frame falls from 12.318 to
8.007 ms: the full redraws (2.59 ms of local shadows and 2.51 ms of
transmission layers) give way to 0.230 ms of copies and 0.552 ms of dynamic
draws for about 12 faces. A motionless dynamic caster at the Ultra budget
matches the full redraw within the snapshot gate with identical depth; at
the default budget the full redraw showed the mannequin's shadow for 5 of
the 9 lamps it reaches, the static and dynamic squares for all 9. A Debug run
with the validation layer's synchronization checks reports no message.
`test_same_image_layer_copy_barriers` and the shadow-system tests cover the
layer copy's barriers, the face mask, reuse, a caster leaving, a static
change, the fallbacks, an offscreen face, the cap and validation.

Static and dynamic squares on the tiled pipeline, Metal Release, Apple M1 Pro,
2026-10-10, local and non-authoritative on a dirty tree, against `32656d25`
(before the split) built in a separate worktree:
- `local_shadow_bistro_metal_street_moving` (`local-offscreen-gpu-single`,
  300 frames, runs alternated after/before/after/before): four faces copied
  and four drawn dynamic per frame, none redrawn whole, none dropped. The
  local shadow passes fall from 8.19 ms (24 face redraws) to 0.40 ms of
  dynamic draws; the copies are blits without timestamps. Frame wall time
  falls from 26.8 to 18.1 ms at p50 in both pairs.
- `local_shadow_bistro_metal_street_moving_capture` before and after: depth
  identical; colour differs only in the clouds (1,101 of 1,103 pixels above
  2/255), which follow submission history: the two binaries reach frame 568
  through different submit counts. A rerun of the same binary differs in
  6 pixels.
- One `MTL_DEBUG_LAYER=1` snapshot of the moving capture case reports no
  message from the copies or the dynamic draws.

Native Metal Release checks on 2026-09-07, on the Metal desktop implementation
removed on 2026-10-06, validated six 64² scene captures, exact face/quadrant
colors in the saved RGBA16F cube, and current/stale source detection.
A local-only reload produces colored diffuse and specular lighting across opaque,
blended and transmitting swatches. This exposed and fixed the prior dependency on
a global environment: ready local probes now enable IBL, and Metal binds a defined
black global cube when no current global source exists.

In the same runs, with 48 static cooked casters, the measured frame emitted
zero local-shadow passes after warmup; classifying the same geometry dynamic
forced all 16 passes. Final capture bytes were identical. The existing
procedural-cube fixture remains dynamic and correctly redraws. CPU checks cover
selection hysteresis, complete point groups, per-image and revision
invalidation, dynamic overlap, and cancelled submissions. These are
correctness and work-count observations, not a matched performance claim.

On 2026-09-26 the atlas replaced the 512-squared depth array. On the Metal
desktop implementation (M1 host, Bistro street view), forcing every face to 128
squared changed up to 22% of each view's pixels, which confirmed that
receivers sample the atlas squares, and larger faces added about 0.2 ms to
`Shadow.LocalMask` through texture-cache pressure. CPU tests cover disjoint
aligned packing when 18 faces at 1024 squared overflow the atlas, and the
face-size hysteresis. All 31 Vulkan modules that declared the view record
passed `spirv-val` with the atlas square at byte 128.

On 2026-10-01 the mask became per-pixel overlap slots, the face capacity rose
to 64 and Ultra was added. Native Vulkan Release on an AMD Radeon RX 6700 XT
(driver 26.6.3, Ryzen 5 2600) ran the Bistro street view at 1280x720 with
`local_shadow_bistro_vulkan_street` and the non-authoritative
`local-offscreen-perf-audit-gpu` profile, three children of 300 frames each,
alternating the pre-change and changed builds of the same dirty tree with
matching workload and environment fingerprints. Steady state reuses every face,
so no face pass runs. `Shadow.LocalMask` stayed at 1.51 ms median and
`Lighting.Deferred` at 1.34 to 1.37 ms. Skipping reused faces lowered
`Cull.Classify` from 0.038 to 0.043 ms to 0.016 ms. Median frame wall time
was 5.32 ms for the second pre-change run and 5.27 to 5.28 ms for the changed
build; the first pre-change run was unstable (8.5 ms, 2.1 ms deviation).
Ultra in the same view
added 0.025 ms of mask time, 0.06 ms of deferred lighting and 0.13 ms of frame
time over High, and its 256² transmission pool cut render-graph images by
315 MiB. These are local observations, not authoritative speed claims. The
pre-change and changed High final-color captures differ in 13 of 921,600
pixels, none by more than 8 of 255, less than two captures of one build
differ. An Ultra capture adds lamp shadows the High selection leaves out. A
Debug run of the Ultra case under `VK_LAYER_KHRONOS_validation` with
synchronization checks reported no messages while drawing all 60 faces and
then reusing them. CPU tests cover ten point lights filling the Ultra budget
beyond the eight mask layers and the transmission pool bound.

Contribution ranking was measured the same day on the same host with
`local_shadow_bistro_vulkan_walk` and its Ultra variant: an 18-second walk
through four Bistro street viewpoints at about 2 m/s, two children each under
`local-offscreen`, the same build run with and without
`VKR_LOCAL_SHADOW_FEEDBACK=0`. The metrics weight each light by its measured
contribution: `lighting.local_shadow.unshadowed_ratio` is the share of visible
local light whose shadow is missing or partial, and
`lighting.local_shadow.fading_ratio` the share whose shadow is crossfading.
With High, contribution ranking cut the unshadowed share from 43% to 26%; the
crossfading share averaged 2.0% against 1.4%, with a 95th percentile of 8.6%
against 10.4%. With Ultra, the unshadowed share fell from 23% to 13% and the
crossfading share averaged 0.9% against 0.6%, with a 95th percentile of 3.9%
against 8.0%. Contribution ranking swaps lights of similar contribution more
often but fewer at once; the 1.5 incumbent preference used with it lowered
Ultra's average crossfading share from 1.3% and its 95th percentile from
4.9%. Against High with distance ranking, Ultra with contribution ranking
leaves 13% of visible local light unshadowed instead of 43% and crossfades
0.9% of it on average instead of 1.4%.

On 2026-10-02 the cache replaced per-frame selection. On the Metal desktop
implementation (M1 Pro, Release, Bistro indoor camera, High preset, 40 warmup
frames), `lighting.local_shadow.lights` read 72 with none fading: every Bistro
lamp was shadowed after the fill. CPU tests cover reuse, invalid and stale
content, cleared and replaced pools, transmission layers, the fill within the
budget, the fade-in and camera cuts. All 104 Vulkan modules passed
`spirv-val`; matched Vulkan timings of the cache remain unmeasured.

The same day on the Metal desktop implementation (M1 Pro, Release, 1280x720,
High preset), `local_shadow_cache_bistro_metal_indoor` (owner camera 1) and
`local_shadow_cache_bistro_metal_street`, Metal desktop cases since removed,
ran once each under `local-offscreen-gpu-single`, 60 warmup and 120 measured
frames, with `VKR_LOCAL_SHADOW_FADE_DISTANCE` varied. Indoors, frame wall time
was 12.9 ms median without local shadows (fade 0.001 m) and 16.2 to 16.4 ms
with 40, 61 or 72 lamps shadowed (fade 15 m, 30 m, 1000 m); `Shadow.LocalMask`
took 2.8 to 3.0 ms and `Lighting.Deferred` 3.6 ms against 3.4 ms. In the street
view, frame time was 12.2 ms without local shadows and 19.0 to 19.2 ms with 41,
56 or 72 lamps (fade 20 m, 30 m, 1000 m), where local shadows added 5.1 ms of
mask, 1.7 ms of `World.Blend`, 1.0 ms of deferred lighting and 0.65 ms of
transmission shading. The fade distance does not change the cost in either
view: nearby lights covering many pixels dominate. Faces of 256 squared instead
of 512 lowered street mask time from 5.09 to 4.92 ms and left the indoor view
unchanged. These are single local observations, not matched speed claims.

Skipping lights behind the shadow normal was measured the same day with
alternating builds of the change on the Metal desktop implementation (M1 Pro,
Release, one run each under `local-offscreen-gpu-single` with
`VKR_LOCAL_SHADOW_FADE_DISTANCE=1000`). In the indoor walk through the five
owner indoor cameras, frame wall time fell from 20.16 to 19.09 ms median,
`Shadow.LocalMask` from 3.98 to 3.45 ms and `Lighting.Deferred` from 5.22 to
4.94 ms; the street view was unchanged at 6.31 to 6.37 ms of mask and 20.4 to
20.6 ms of frame time in both builds. Captures after the fill were
byte-identical indoors and differed in the street view by at most 8 of 255, as
two captures of one build do.

The unfaded size ranking (2026-10-12) has a CPU test,
`test_local_shadow_fade_keeps_face_size` in
[`shadow_system_test.c`](../../tests/src/shadow_system_test.c): of two
lights over a one-layer budget the nearer takes 1024 squared and the other
512, and fading the nearer to a tenth of its intensity must neither redraw
nor swap the sizes. It fails with the faded intensity, and the Debug CPU
suite passes on Windows. No native run has checked a Bistro night view past
the atlas layer budget with more than 128 lights for face resizes while
lights fade.

Idle-mover reuse, the atlas budget, the ranked table and the mask gate
(2026-10-11) ran on native Vulkan on 2026-10-12: Release, RX 6700 XT, driver
26.6.3, Windows 10 19045, Ryzen 5 2600, a dirty tree on `4eaeecbb` and local
profiles, so no result is authoritative. Harness runs used
`build_release/tools/vkr_harness.exe snapshot|profile --case tools/cases/local/<case>.case.json --profile tools/profiles/<profile>.json`
with the graphics validation variables unset.

- `local_shadow_bistro_vulkan_street_motionless_capture` (snapshot,
  `local-offscreen`, Ultra, 1280x720, 120 warmup frames and one measured
  frame). A copy that differed only in a shorter description ran on the
  change set's first build
  (`sha256:b6b71cd43db950b30da02f8aafc9740f03aade2d1cc80d1c6c5b9c0dccabe48f`):
  12 faces wanted a dynamic square and none drew on the measured frame,
  dynamic squares drew only in warmup frames 1, 2 and 10 (10, 2 and 1
  faces), no face was dropped after frame 1, the cascades were reused and all
  72 lamps were shadowed. The tracked case, whose description now fits the
  manifest's 255 bytes, passed on the final tree with all six child
  assertions, among them `draw.local_shadow.faces_dynamic` at most 0
  (`sha256:50c5e5b1c9cbd953874097ea86a4518815d6c3c571c79af215f61930433cf2da`).
  No capture compared a kept square with a fresh draw.
- Builds before (B) and after the change set (A, and A' after a rebuild)
  alternated under `local-offscreen-perf-audit-gpu`, two children each. In
  `ssr_tier_bistro_cost` (since deleted with SSR; 640x360, TAA, 24 warmup and
  32 measured frames, 48
  to 72 shadowed lights while their shadows fade in) `Shadow.LocalMask` read
  0.178 ms in B against 0.125 ms in A and 0.120 ms in a repeat of A', beyond
  the per-child spread (`sha256:b6373852f3be`, `sha256:a6b37e05f249`,
  `sha256:d9c6935044f2`). In `local_shadow_bistro_vulkan_walk_ultra`
  (1280x720, no TAA, 240 warmup and 1,086 measured frames) it read 1.909
  against 1.958 and 1.908 ms, within spread (`sha256:c1b3b735bca4`,
  `sha256:b809f9367308`, `sha256:9dfda34f0ad8`), and in the sparse-volume
  case at 2560x1440 5.04 against 5.10 and 5.04 ms. Work rows (`draw.*`,
  `lighting.*`, `visibility.*`) matched in every phase. These figures time the
  whole change set on a dirty tree: local observations, not speed claims.
- Bistro selected 72 lights and dropped none, and its render-graph images
  stayed at 1,391.62 MiB in every phase of the walk, so its atlas did not
  change. The walk draws local faces on 26 of its 2,172 measured frames, so
  it exercises neither the reuse nor the budget.

The Testbed project's "Level Design Test" scene (226 point lights, 111 movers
as dynamic casters, Epic preset) is not a harness case. A headless Release editor
(`build_release/editor/vkr_editor.exe`, `--metrics-json` exit frames,
`VKR_RG_GPU_TIMING=1`, validation variables unset) showed it with the change
set. The before values are single runs of 2026-10-11 on `4eaeecbb` with a
dirty tree, not a matched alternation, and other processes loaded the host
at times. These figures are indicative. They are not harness reports.

- At the lobby, at rest in Edit mode, no local face was copied or drawn
  dynamic (16 of each before); 23 faces wanted a dynamic square and 7 past
  the 16-face cap showed their static square, as before. With the cascades
  (ADR-041) the frame ran 86 passes instead of 128, and the maximized Scene
  view's exit frame summed 11.37 ms of pass time with no cascade, face or
  moments pass, against 12.55 ms with 1.26 ms of them.
- The table selected 128 lights and dropped 98 at every view, and 120 lights
  were shadowed (114 before).
- At the bridge view the gate skipped `Shadow.LocalMask` (0.12 ms before).
- The Draws window at the lobby showed render-graph images at 1,315.89 MB
  instead of 1,795.89 MB. The atlas layer count was not read, so the atlas
  budget's share of that fall is inferred, not measured.

CPU tests in [`shadow_system_test.c`](../../tests/src/shadow_system_test.c)
cover a kept dynamic square and its loss after a move, a static redraw or a
band clear (`test_local_shadow_idle_dynamic_square_is_reused`), the band
beside 31 static layers (`test_local_shadow_band_survives_full_static_layers`),
residency and size hysteresis past the budget
(`test_local_shadow_atlas_budget_ranks_residents`) and Bistro's lamps under
every preset (`test_local_shadow_bistro_lamps_fit_every_preset`);
[`render_graph_barrier_test.c`](../../tests/src/render_graph_barrier_test.c)
covers the mask gate (`test_local_shadow_mask_gate`) and
[`lighting_system_tests.c`](../../tests/src/lighting_system_tests.c) the
ranked table. The Debug CPU suite passes on Windows (2026-10-12).

Unverified: Metal runs of the reuse, the budget and the ranked table, which
share the runtime code; a Bistro fixture past 128 lights, so the ranked
table, its fades and the `light_overflow` level check ran in no harness case;
a Bistro case past the atlas budget; a harness case that skips
`Shadow.LocalMask`, since no Bistro case faces away from every lamp;
capture equivalence of kept squares and of the gate; authoritative
clean-tree timing.

