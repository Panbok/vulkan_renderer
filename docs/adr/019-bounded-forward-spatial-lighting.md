---
status: implemented
updated: 2026-10-04
authority: adr
---

# ADR-019: Bounded punctual lighting, local probes, and cached local shadows

## Status

Accepted.

## Context

A scene-wide light prefix drops lights unpredictably, and selecting one probe
per draw causes large meshes to inherit the wrong local environment.

## Decision

Use a stable table of up to 128 punctual lights and a conservative 384-cell
fragment-local bitmask grid. CPU scene synchronization owns membership; shaders
perform exact range and spot-cone rejection. Four-vector, 64-byte light rows
share position/range, direction/cone, color/intensity and type semantics across
Metal and Vulkan shading paths.

Pack up to 16 ready local IBL probes per frame. Compute their influence from
fragment position and AABB weights; keep the global environment as fallback.
Box projection controls only parallax-corrected reflection lookup. Disabling it
does not bypass a local probe's influence extents or blend falloff. The global
environment remains the unbounded source; existing application packing enables
box projection, so this correction does not migrate current authored probe values.
Diffuse uses ADR-038 coefficients and specular uses prefiltered cubemaps.
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
[local shadow architecture](../proposals/local-shadow-architecture.md). Every
shadow-casting light of the scene table is resident: a spot owns one
perspective face and a point six, in +X, -X, +Y, -Y, +Z, -Z order. Each face
is a square of one shared 4096-squared D16 atlas array that every frame in
flight samples. A face's side follows only its light's range: the power of two
nearest the range times 64 texels per metre, from 128 to the preset's largest
face, so a 7.5 m Bistro lamp takes 512 squared and the 15 m lamp 1024 squared
under High, and camera motion never resizes a face. The atlas takes as many
layers as its resident faces need, at most 32; the lowest-importance lights
shrink only if they would exceed that. Squares pack in descending size by layer
and first free aligned cell; power-of-two squares that fit by area always pack.
A light whose size and layer count are unchanged keeps its squares.

The atlas is D16 on both backends: Metal render pipelines carry no depth
format, and Vulkan draws local faces with D16 variants of the shadow pipelines
and requires D16 depth attachment, sampling, comparison and linear filtering
(ADR-023). Receivers keep their texel-footprint bias. On Bistro (Metal Release,
M1 Pro, 2026-10-03) it halved the atlas from 768 to 384 MiB (12 layers) with
unchanged redraw counts and local-shadow pass times, and final colour against
D32 stayed within the default snapshot gate in the street overview (1 pixel
above 10/255), a street-level facade view (0.011% of pixels above 2/255) and
`bistro_bright_spot_snapshot` (maximum 3/255). On Vulkan (RX 6700 XT, driver
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
reaches the light's range sphere, or the caster publication generation
changed (completed texture, sampler and material publications; a geometry
publication cannot change a drawn caster, since a mesh draws only after its
geometry has published and adding or removing a drawn mesh is a static
change). The world payload lists each static change with the world box it
may alter (`VkrWorldPassPayload.static_changes`): a mesh's arrival or
departure its bounding box, a terrain swap the footprints of the tiles that
changed. A change the list does not bound, or content older than the list,
reaches every light. Content no change reaches takes the new generation, a dynamic caster's bounds reach the light, the
dynamic-bounds scan is unavailable, or an asset publication is in flight.
Stale content keeps showing while it waits to redraw. The preset's face budget
bounds the faces drawn per frame: High 30, Balanced 12 and Ultra 60. Complete
lights draw in that budget, invalid lights first, then lights waiting for
transmission layers, then stale ones, each by importance. A light is shadowed
once every face is valid; its strength then rises from zero over 0.25 s, or at
once on the first resolve, a budget change and a renderer camera cut (the TAA
rule: more than 10 m or 60 degrees of turn), since the image has no history to
keep. A resident light whose content stays valid never fades out with time, so
its shadow cannot switch off while the camera moves. Receivers instead see that
strength times a fade by camera distance, from one to zero over the last 5 m
before `VkrShadowConfig.local_shadow_fade_distance`, 120 m by default: a light
past it is not shadowed, so no pixel filters it, yet it stays resident and its
shadow returns continuously as the camera approaches. Faded lights draw last.
The default was 30 m until 2026-10-02. Because residency does not depend on
it, the distance bounds only filtering, and distant lights cover few pixels
and take the single tap. On the M1 host (Metal Release,
`local-offscreen-gpu-single`, matched builds), 120 m shadowed all 72 Bistro
lamps instead of 56 in the street view and 62 in
`local_shadow_cache_bistro_metal_indoor_walk`, with `Shadow.LocalMask`,
the GPU pass sum and median frame time unchanged within 0.05 ms, with and
without TAA. In that walk the unshadowed share of visible local light fell
from 0.42% mean and 6.0% at most to zero; the street and indoor captures
stayed within 6 of 255.
`VKR_LOCAL_SHADOW_FADE_DISTANCE` overrides the distance for diagnosis. Shadowed lights require a
finite positive range, and shadowed spot outer half angles must be below 90
degrees.

Importance orders the fill and the filter. It is luminance times intensity
times range squared over the squared camera distance, held constant within a
tenth of the range, so the nearest of several overlapping lights wins. Distance
cannot tell a lamp lighting the street from one enclosed in a building, so
importance is instead each light's measured visible contribution once a sample
exists: deferred lighting on both backends sums, over the pixels it shades,
the light's unshadowed luminance at the surface after pre-exposure, x,
compressed per pixel to x / (1 + x) by the shared
`vkr_local_light_contribution`, with one atomic per SIMD group or wave per
light into counters the frame reads back: a per-frame-slot buffer on Vulkan,
the frame's cleared readback slice on Metal. The readback reaches the cache
two to three frames later as a `VkrLocalLightContributionSample` keyed by
render id. Deferred lighting measures only in frames whose index is a
multiple of `VKR_LOCAL_LIGHT_CONTRIBUTION_PERIOD`, four, which the shared
`light_contribution_enabled` frame flag selects for both backends
([`vkr_render_graph_frame.c`](../../renderer/src/vkr_render_graph_frame.c)),
so the newest sample is at most about six frames old. On Vulkan, RX 6700 XT,
in the Bistro street view at 1920x1080 with TAA, mean `Lighting.Deferred`
time fell from 3.68 to 3.54 ms and the final color did not change. On Metal
the flag clears the counter address, which the deferred kernel tests before
its atomics. On the M1 Pro (Release, `local_shadow_cache_bistro_metal_indoor_walk`,
`local-offscreen-perf-audit-gpu`, two children of 660 frames, non-authoritative),
a period of one against four read mean `Lighting.Deferred.Fullscreen` 2.284
against 2.257 ms with a 0.22 ms spread, within noise, and identical
`lighting.local_shadow.*` values (72 lights, no fading).
Shadowing never changes the measure. A sample is used only when it
is at most eight frames old and comes from no earlier than the last snap;
otherwise distance ranks. `VKR_LOCAL_SHADOW_FEEDBACK=0` restores distance
ranking for diagnosis.

A light whose unshadowed contribution at a pixel, the shared
`vkr_local_light_contribution` taken with the shadow normal, is below
`VKR_LOCAL_LIGHT_CONTRIBUTION_CUTOFF` (0.001 of pre-exposed luminance) takes no
mask slot and no deferred shading; `Shadow.LocalMask` and deferred lighting
apply the same test, so their slots stay in step, while forward and
transmission shading keep every light. Because the threshold follows
pre-exposure, it means the same display contribution by day and night. In
matched Metal runs on the M1 host it lowered `Lighting.Deferred` by 0.33 ms in
the Bistro street view, with and without TAA, and by 0.22 ms in
`local_shadow_cache_bistro_metal_indoor_walk`, partly because fewer pixels
overflow their eight slots into inline filtering; the GPU pass sum fell by
0.31 to 0.38 ms. Lit surfaces changed by more than 2 of 255 on at most 41
pixels of the street and indoor captures (cloud pixels moved by up to 40
through the reordered sky evaluation under fast math). Skipping such lights
saved nothing in the mask, whose single-tap cost comes from lights that matter.

Each shadowed light adds its filtering to every pixel in its range. The two
most important shadowed lights take the nine-tap filter and contact shadows;
the others take one hardware-filtered comparison tap and no contact shadows,
flagged by `shadow_params.z`. A light that had the full filter keeps a 15%
incumbent preference for it, so its filter does not flip while scores cross.
Before the cache, on the M1 development host at 1280x720 in the Bistro street
view, five full-filter lights cost 3.0 ms more `Shadow.LocalMask` time and
3.8 ms more frame time than three, and four cost 1.5 ms and 2.2 ms more; five
lights with two reduced cost 0.6 ms more mask time and 0.95 ms more frame time
than three, because forward and transmission shading also filter the extra
lights. On 2026-10-03 the count fell from three to two
([`vkr_local_shadow_system.c`](../../runtime/src/renderer/systems/vkr_local_shadow_system.c)).
On Vulkan, RX 6700 XT, in the Bistro street view at 1920x1080 with TAA and
the High preset (`local_shadow_bistro_vulkan_street` at that size, one child
of 240 frames under `local-offscreen-gpu-single`), `Shadow.LocalMask` fell
from 2.84 to 2.66 ms and the GPU frame from 11.15 to 10.94 ms. The final
color changed in 0.14% of pixels by more than 8 of 255, at the shadow edges
of the third light, and a repeated capture of one build changed none. On the
M1 Pro (Metal Release, 1280x720, TAA, `local-offscreen-perf-audit-gpu`, two
children each, non-authoritative), three against two lights read
`Shadow.LocalMask` 3.094 against 2.805 ms and median frame 14.71 against
14.33 ms in `local_shadow_taps_bistro_metal_street_taa`, and 1.314 against
1.249 ms and 11.70 against 11.50 ms in
`local_shadow_taps_bistro_metal_indoor_walk_taa`. The street capture changed
in 0.10% of pixels by more than 8 of 255 (at most 33), along one pillar edge
and not visible side by side, against a repeat floor of 5; the indoor capture
changed at most 4.

Under temporal reconstruction `Shadow.LocalMask` filters the full-filter
lights with the first four taps of the progressive Poisson table instead of
nine, rotated per pixel by interleaved gradient noise that advances with the
contact-shadow noise index, and TAA integrates the rotations
([`local_shadow.slangh`](../../renderer/src/shaders/shared/local_shadow.slangh)).
Without temporal reconstruction the mask keeps the fixed nine taps, and
lighting's inline fallback, the clearcoat query, forward and transmission
shading always do. On the M1 host (Metal Release,
`local_shadow_taps_bistro_metal_street_taa`, 1280x720, TAA, profile
`local-offscreen-gpu-single`, timestamps on) the mask fell from 6.44 ms to
4.61 ms and the frame from 20.2 ms to 17.6 ms median; in
`local_shadow_taps_bistro_metal_indoor_walk_taa` (0.75 scale) the mask fell
from 2.03 ms to 1.65 ms. The street capture after TAA changed at most 9 of 255,
with 0.26% of pixels changing by more than 2, against a repeat floor of 3; the
indoor capture changed at most 1. Without TAA the street capture is
byte-identical. Native Vulkan remains unrun.

Forward and transmission shading filter full-filter lights inline with the
first four progressive taps, unrotated (`VKR_LOCAL_SHADOW_INLINE_TAP_COUNT`).
They shade few pixels, so rotation would add plumbing without visible gain:
on the M1 host transmission shading fell from 1.94 to 1.53 ms in the street
view (1.99 to 1.56 ms with TAA, 0.94 to 0.86 ms indoors) with at most 0.04%
of pixels changing by more than 2 of 255. A single tap saved 0.23 ms more but
changed 0.24%. Lighting's inline fallback and the clearcoat query keep nine
taps.

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
weighted by the incumbent preference, hold a layer for every face; refractive
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
its crossings together. Its transmission is valid only with matching
generations for all five arrays and valid contents in each of its layers; an
incomplete or replaced prefix pool redraws the complete light, which keeps its
opaque shadow meanwhile. Cancelled work never promotes history. Scenes with no
refractive candidates allocate no transmission pool or views.

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
coefficients and prefilter. Source mip count comes from the finalized texture;
a one-mip saved source therefore projects SH from mip 0 and prefilter sampling
falls back to mip 0. The runtime does not render six scene views for a saved
probe. A probe with no cubemap continues to alias the ready scene environment.

[`vkr_bakery bake probe`](../../tools/bakery/vkr_bakery_bake.c) owns offline
six-view capture in one harness session. A bake records
the source scene manifest, per-face capture and report digests, output digest,
and one stable native provenance record in `<output>.bake.json`. It captures the
six KTX cube faces in `+X, -X, +Y, -Y, +Z, -Z` order with local probes disabled,
then the packer vertically flips each top-left capture exactly once. The emitted
`.vkt` is an uncompressed KTX2 cube with six RGBA16F faces, one array layer, and
one mip level. It has no runtime-generated source mips or orientation repair.

Example bake and freshness check:

```sh
./build_release/tools/bakery/vkr_bakery bake probe \
  --scene assets/scenes/example.json \
  --position 0 1 0 \
  --output assets/probes/example.vkt

./build_release/tools/bakery/vkr_bakery bake probe \
  --output assets/probes/example.vkt --check
```

Receivers use nine comparison-PCF Poisson taps with a 1.5-texel radius, in
texels of the face that owns each tap. Normal offset is two local shadow texels
and receiver bias is one texel, converted using the perspective footprint at
receiver depth. Point taps reconstruct rays and reproject into adjacent faces. A
tap that stays inside the receiver's face derives its reference depth from the
face projection instead of reprojecting. Each tap maps its face UV into the
face's atlas square, clamped half a face texel inside it so bilinear
comparison never reads a neighbouring face.
Both deferred and transparent lighting consume the same local visibility
semantics.

Deferred lighting does not filter local shadow maps itself. The
`Shadow.LocalMask` compute pass runs before it when local shadows are active
and, for every opaque pixel, stores the filtered RGB visibility, with strength
applied, of the k-th shadowed light in range of the pixel that can light it, in
light traversal order, in layer k of an eight-layer full-resolution RGBA8
array. A light can light the pixel when it lies in front of the shadow normal,
which back-lit diffuse transmission flips toward it
(`vkr_local_shadow_light_faces`); otherwise every lobe that uses the base
visibility is zero, so no receiver looks it up and the mask gives it no slot.
A coat whose normal differs keeps its own inline lookup. Alpha tags the
layer with the light index plus one over 255. The pass repeats deferred
lighting's world-position reconstruction, light traversal, range and cone
tests, back-lit normal choice and facing test, so lighting counts the same
lights in the same order and reads layer k for its k-th shadowed light. A light past the
eighth, or whose layer tag names another light because a boundary test
differed between the passes, is filtered inline by lighting without contact
shadows. Eight layers therefore bound the shadowed lights overlapping a pixel,
not the shadowed lights in the frame. The clearcoat's second visibility query for a differing coat normal,
forward shading, and transmission shading still filter inline. On the M1 host in
the Bistro street view, the mask pass costs about 2.3 ms and lowers
`Lighting.Deferred` from 6.7 ms to 3.3 ms, a net 1.1 ms of GPU time. Output
matches inline filtering within 8-bit quantization of visibility. A
half-resolution RGBA16F mask with depth- and normal-aware upsampling was
measured and rejected: at equal quality it was slower than the full-resolution
mask, and without an inline fallback it lost sharp local shadow detail on thin
geometry near lights.

The mask pass also applies contact shadows to each shadowed light. From a
start offset along the shadow normal, it marches eight depth-buffer samples
over at most 0.25 m toward the light, and never more than half the distance
to it. A sample is occluded when the surface on its camera ray lies in front
of it by more than 0.4% of the camera distance but less than 3 cm plus 0.5%
of it. The thin limit keeps rays that pass behind lamp frames and other
nearby geometry unoccluded. Occlusion in the far half of the ray fades out.
The result is blended by the light's strength and multiplies its filtered
visibility, so contacts the map's filter radius, normal offset and bias
remove, such as vines on a wall, keep a shadow. The march start follows an
R2 sequence over the pixel that advances with the frame index when temporal
reconstruction is enabled, so TAA integrates the step pattern. Contact
shadows are screen-space: occluders off screen or hidden behind nearer
surfaces cast none, and forward and transmission shading do not apply them.
They add about 0.85 ms to the mask pass in the Bistro street view on the M1
host. With every lamp shadowed (56 lights in that view, 2026-10-02, TAA off)
removing the march saved 1.49 ms of the 6.34 ms mask, but four steps instead
of eight saved only 0.22 ms, and one march per pixel after the light loop,
toward the full-filter light with the largest shadowed contribution, saved
0.04 ms while dropping the other lights' contacts, so it was rejected.
Replacing each step's world reconstruction with the ratio of view depths along
the sample's camera ray, one dot product instead of a 4x4 transform, left the
mask at 6.44 ms and was not kept either. The cost follows the march's presence
in the kernel rather than its arithmetic.

A Metal System Trace with GPU counters of the street view, with the mask in
its own encoder, explained why: the mask ran at 18.5% compute occupancy with
no limiter above 40% (ALU 40%, texture sampling 13%, last-level cache 13%), so
it is latency-bound, and the compiler allowed it only 576 threads per
threadgroup, 640 without the march. Base deferred lighting was allowed 384
and ran at 20%. On Metal the two kernels therefore carry
`max_total_threads_per_threadgroup` hints of 768 and 512
([`gpu_draws.metal`](../../renderer/src/shaders/metal/msl/world/gpu_draws.metal));
sweeps from 640 to 1024 and from 448 to 640 found these optima, past which the
compiler spills. In matched runs (Metal Release, `local-offscreen-gpu-single`)
the mask fell from 6.47 to 5.74 ms and lighting from 4.12 to 3.74 ms in the
street view without TAA, the GPU pass sum from 19.20 to 18.01 ms and the frame
median from 20.9 to 19.5 ms; with TAA the pass sum fell from 17.02 to 16.17 ms
and in `local_shadow_cache_bistro_metal_indoor_walk` from 11.52 to 10.99 ms.
The hints change only code generation; under fast math the reordered sky
evaluation moved cloud pixels by at most 20 of 255 (0.11% of the street view
by more than 2), and every lit surface matched. With the hint the march still
costs 1.1 ms (5.78 against 4.67 ms without it). A separate
`Shadow.LocalContact` pass that rescanned the mask slots and marched for the
full-filter lights was built on both backends and measured on Metal: the mask
fell to 4.64 ms but the contact pass took 1.36 ms, and 1.18 ms with the
view-depth march and its depth reads issued before any test, so mask and
contact together stayed above the single kernel (5.84 against 5.74 ms). It was
not kept; the march costs about 1.1 ms wherever it runs, and only fewer
marches or steps, a quality decision, would lower it. Vulkan has no equivalent hint; its drivers choose

Contact shadows therefore belong to the Ultra preset only
(`VkrShadowConfig.local_shadow_contact`, carried to the backends as
`VkrLocalShadowPassPayload.contact_shadows`). Both backends build the mask
twice, `vkr_metal_packet_local_shadow_mask[_contact]` and
`vk_local_shadow_mask[_contact]`, so the default kernel carries none of the
march's registers. In the application they follow the "Contact shadows"
graphics setting on both backends, which the Epic preset enables, so Metal
can render them under High's local-shadow budget; harness cases enable them
through the `ultra` shadow preset. On the M1 host (matched builds, default
preset) the mask fell from 5.68 to 4.61 ms in the street view without TAA and
from 4.18 to 3.15 ms with it, the frame median from 16.3 to 15.3 ms with TAA,
and indoors from 1.77 to 1.60 ms; an Ultra capture was unchanged.
occupancy. Probe bounds/ranges are not geometry visibility. The removed hard influence-AABB
experiment is not an occlusion mechanism. GTAO attenuates local indirect diffuse
only and does not establish arbitrary wall or furniture occlusion.

## Consequences

Lighting is bounded and independent of draw partitioning. Unshadowed lights and probe bounds can still leak illumination through geometry.
Every resident face costs atlas memory whether or not it is on screen: each
4096-squared D16 layer is 32 MiB, one image for all frames in flight. Bistro's
71 lamps of 7.5 m take 426 faces at 512 squared and its 15 m lamp six at 1024
squared under High, eight layers or 512 MiB; the per-image atlas it replaces
took 64 MiB per target image. These are storage figures from the layout, not
a measured allocation. A face redraws whenever its content is invalid or
stale, within the face budget per frame; after the fill, cost scales with
dynamic-caster overlap and moved lights. A scene load draws its faces over
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
uncompressed RGBA16F KTX2 cube with the baker's face order and one-mip storage
contract.

## Alternatives considered

A first-N global list makes visibility depend on source order. Per-draw probe
selection fails on large meshes. Hard light influence boxes produced discontinuous
slabs without solving diffuse occlusion. Excluding glass from depth-only shadows
would restore light but discard tint and attenuation. Four transmitting crossings
would add 624 MiB across three full-pool target images and five passes per
refreshed face; the accepted budget is two crossings and an opaque overflow
boundary to prevent leaks.

## Revisit when

The remaining phases of the
[local shadow architecture](../proposals/local-shadow-architecture.md) land:
mask filtering bounded by a distance fade, Metal contribution measurement,
measured cache format and face size, residency for open worlds. Scene scale exceeds
these capacities, stored probe source requirements change,
or local-light transport requires more crossings, refracted caustics, or a
different memory or raster budget.

## Implementation

[`scene_loader.c`](../../runtime/src/renderer/resources/loaders/scene_loader.c)
parses and owns direct source loading. [`vkr_world_resources.c`](../../runtime/src/renderer/systems/vkr_world_resources.c)
requests the one-time probe bake. [`vkr_bakery_bake.c`](../../tools/bakery/vkr_bakery_bake.c)
and [`vkr_hdr_cube_packer.cpp`](../../tools/vkr_hdr_cube_packer.cpp) define the
offline artifact and provenance contract. [`vkr_shadow_system.c`](../../runtime/src/renderer/systems/vkr_shadow_system.c),
[`vkr_standard_scene_runtime.c`](../../runtime/src/application/vkr_standard_scene_runtime.c),
and the Metal/Vulkan retained graph providers implement local-shadow reuse and
submit-only promotion.

## Evidence

Native Metal Release checks on 2026-09-07 validate six 64² scene captures, exact
face/quadrant colors in the saved RGBA16F cube, and current/stale source detection.
A local-only reload produces colored diffuse and specular lighting across opaque,
blended and transmitting swatches. This exposed and fixed the prior dependency on
a global environment: ready local probes now enable IBL, and Metal binds a defined
black global cube when no current global source exists.

With 48 static cooked casters, the measured frame emits zero local-shadow passes
after warmup; classifying the same geometry dynamic forces all 16 passes. Final
capture bytes are identical. The existing procedural-cube fixture remains dynamic
and correctly redraws. CPU checks cover selection hysteresis, complete point groups,
per-image and revision invalidation, dynamic overlap, and cancelled submissions.
A single focused Metal API-validation process covering saved probes and retained
local shadows passes. These are correctness and work-count observations, not a
matched performance claim. Native Vulkan execution remains unavailable on macOS;
Metal GPU shader validation remains unresolved as recorded in ADR-044.

On 2026-09-26 the atlas replaced the 512-squared depth array. In the Bistro
street view on the M1 host, the High preset gave two point lights 1024-squared
faces and the third 512-squared once the atlas filled. The Bistro Metal text
snapshot still passed its accepted baseline, and forcing every face to 128
squared changed up to 22% of each view's pixels, which confirms receivers
sample the atlas squares. Larger faces added about 0.2 ms to `Shadow.LocalMask`
through texture-cache pressure. CPU tests cover disjoint aligned packing when
18 faces at 1024 squared overflow the atlas, and the face-size hysteresis. A
single Metal API-validation process passed, and all 31 Vulkan modules that
declare the view record pass `spirv-val` with the atlas square at byte 128.
Native Vulkan execution remains unavailable.

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
beyond the eight mask layers and the transmission pool bound. Metal sources
changed in step but were neither compiled nor run on this host.

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

On 2026-10-02 the cache replaced per-frame selection. On the M1 Pro, Metal
Release, `local_shadow_cache_bistro_metal_validation` (Bistro indoor camera,
High preset, 40 warmup frames) under the serial windowed Metal validation
profile with `MTL_DEBUG_LAYER=1` reported `Metal API Validation Enabled`, no
messages, and `lighting.local_shadow.lights` of 72 with none fading: every
Bistro lamp shadowed after the fill. The Bistro Metal text snapshot ran
without errors; its views show every lamp's shadow, so they no longer match
the accepted baseline, which predates the cache. CPU tests cover reuse,
invalid and stale content, cleared and replaced pools, transmission layers,
the fill within the budget, the fade-in and camera cuts. All 104 Vulkan
modules pass `spirv-val`; native Vulkan execution and matched timings remain
unmeasured.

The same day on the M1 Pro, Metal Release at 1280x720 with the High preset,
`local_shadow_cache_bistro_metal_indoor` (owner camera 1) and
`local_shadow_cache_bistro_metal_street` ran once each under
`local-offscreen-gpu-single`, 60 warmup and 120 measured frames, with
`VKR_LOCAL_SHADOW_FADE_DISTANCE` varied. Indoors, frame wall time was 12.9 ms
median without local shadows (fade 0.001 m) and 16.2 to 16.4 ms with 40, 61 or
72 lamps shadowed (fade 15 m, 30 m, 1000 m); `Shadow.LocalMask` took 2.8 to
3.0 ms and `Lighting.Deferred` 3.6 ms against 3.4 ms. In the street view,
frame time was 12.2 ms without local shadows and 19.0 to 19.2 ms with 41, 56
or 72 lamps (fade 20 m, 30 m, 1000 m), where local shadows added 5.1 ms of
mask, 1.7 ms of `World.Blend`, 1.0 ms of deferred lighting and 0.65 ms of
transmission shading. The fade distance does not change the cost in either
view: nearby lights covering many pixels dominate. Faces of 256 squared
instead of 512 lowered street mask time from 5.09 to 4.92 ms and left the
indoor view unchanged. These are single local observations, not matched speed
claims.

Metal measured contribution from 2026-10-02. With `MTL_DEBUG_LAYER=1`,
`local_shadow_cache_bistro_metal_validation` passed with no messages and
`lighting.local_shadow.unshadowed_ratio` and `fading_ratio` of 0 at the default
30 m fade; with `VKR_LOCAL_SHADOW_FADE_DISTANCE=10` the unshadowed ratio rose to
0.99 with 12 lamps shadowed, which shows the measure reaches the metrics.

`local_shadow_cache_bistro_metal_indoor_walk` walks through the five owner
indoor cameras in 11 s at 1280x720 with the High preset. On the M1 Pro, Metal
Release, one run each under `local-offscreen-gpu-single`:
`lighting.local_shadow.fading_ratio` stayed 0 on every frame, and with
`VKR_LOCAL_SHADOW_FADE_DISTANCE=1000` so did `unshadowed_ratio`; at the default
30 m fade the lamps past it left a mean of 0.4% and at most 5.8% of the visible
local light unshadowed. Frame wall time was 20.4 ms median (25.1 ms p95) with
every lamp shadowed against 14.6 ms (18.2 ms p95) without local shadows, with
`Shadow.LocalMask` at 4.0 ms. Giving every light the single-tap filter lowered
the median to 18.5 ms and the mask to 2.6 ms. The M1 Pro does not hold 60 FPS
in this walk with any measured choice.

Skipping lights behind the shadow normal was measured the same day with
alternating builds of the change on the M1 Pro, Metal Release, one run each
under `local-offscreen-gpu-single` with `VKR_LOCAL_SHADOW_FADE_DISTANCE=1000`.
On `local_shadow_cache_bistro_metal_indoor_walk`, frame wall time fell from
20.16 to 19.09 ms median, `Shadow.LocalMask` from 3.98 to 3.45 ms and
`Lighting.Deferred` from 5.22 to 4.94 ms; the street view was unchanged at
6.31 to 6.37 ms of mask and 20.4 to 20.6 ms of frame time in both builds.
Captures after the fill were byte-identical indoors and differed in the street
view by at most 8 of 255, as two captures of one build do. A
`MTL_DEBUG_LAYER=1` run of `local_shadow_cache_bistro_metal_validation` passed
with no messages. The walk still misses 60 FPS on the M1 Pro.

The local shadow architecture's 60 FPS target therefore renders the M1 family
at a 0.75 render scale. With `render_scale` 0.75 (960x540, spatial upscaling to
1280x720) the same walk measured 12.8 and 12.6 ms median and 16.4 and 16.7 ms
at the 95th percentile in two runs, and 11.1 ms median and 15.0 ms p95 at
0.667, with no crossfading. `local_shadow_cache_bistro_metal_indoor_walk` now
runs at 0.75 and `local_shadow_cache_bistro_vulkan_indoor_walk` keeps full
resolution for the RX 6700 XT; native Vulkan remains unrun. These targets use
1280x720 output. An editor Scene on a 2x Retina display renders about 2.3
times those pixels in the default window and costs proportionally more; the
Graphics setting `high_dpi` renders one pixel per point to compare the two
(ADR-043).
