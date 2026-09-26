---
status: implemented
updated: 2026-09-26
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
opt-in. The High preset budgets 30 faces (five point lights) with faces up to
1024 squared, and Balanced 12 (two) up to 512 squared. Every face is a square
of one 4096-squared D32 atlas. Its side is the smallest power of two, from 128
to the preset's largest face, that reaches the light's range-sphere radius on
screen: half the output height times the projection's vertical scale, times
range over the larger of camera distance and range. A face grows as soon as it
needs more texels and shrinks only once it has twice what it needs, so a camera
near a size boundary does not resize and redraw it every frame. When the faces
exceed the atlas, the lowest-importance lights shrink first; power-of-two
squares that fit by area always pack. A light whose size is unchanged keeps
its squares; the others take the first free aligned squares in descending size,
and fragmentation repacks every face. The pool holds up to 32 faces.

Each shadowed light adds its filtering to every pixel in its range. The three
most important selected lights take the nine-tap filter and contact shadows;
the others take one hardware-filtered comparison tap and no contact shadows,
flagged by `shadow_params.z`. A light that had the full filter keeps the 15%
incumbent preference for it, so its filter does not flip while scores cross.
On the M1 development host at 1280x720 in the Bistro street view, five
full-filter lights cost 3.0 ms more `Shadow.LocalMask` time and 3.8 ms more
frame time than three, and four cost 1.5 ms and 2.2 ms more. Five lights with
two reduced cost 0.6 ms more mask time and 0.95 ms more frame time than
three, because forward and transmission shading also filter the extra
lights. With only three shadowed lights, the nearest won and shadows switched
on only close to the camera. Preferring lights whose range reaches the view
frustum did not help: Bistro's 7.5 to 15 m ranges reach the frustum from
behind the camera, and on-screen selections across the snapshot views rose by
about 4%.

A spot uses one perspective view and a point uses six in +X, -X, +Y, -Y, +Z, -Z order. Importance is luminance times intensity times
range squared over the squared camera distance, held constant within a tenth of
the range, so the nearest of several overlapping lights wins. Selection takes
the complete light groups with the highest total importance, with a 15%
preference for incumbents to limit churn; requests that do not fit remain
unshadowed. At most eight lights are shadowed at once, one per layer of the
screen-space shadow mask described below.

Each selected light carries a shadow strength, and receivers blend its
visibility toward one as strength falls. A selection change crossfades: a light
that loses its place keeps its layers while its strength falls to zero over
0.25 s, and a newcomer takes free layers at zero strength and rises over the
same time, so no shadow switches on or off within a frame. Settled lights stay
at full strength, so co-located lights of similar brightness keep their shadows.
The first selection, a budget change and a renderer camera cut (the TAA rule:
more than 10 m or 60 degrees of turn) snap the desired set to full strength,
since the image has no history to keep. The budget and map size can
be reduced through `VkrShadowConfig`. Shadowed lights require a finite positive
range, and shadowed spot outer half angles must be below 90 degrees.

Application preparation owns the fixed frame-local view payload. Each physical
target image owns its local depth atlas through the existing graph resource and
GPU-completion lifecycle. Each view record carries its atlas square, and frame
validation rejects squares that are unaligned, out of range or overlapping. Local-shadow selection remains bounded by the configured
face budget: a spot owns one view and a point owns six contiguous views. A light
that stays selected keeps its layers; a newcomer takes the lowest free range, and
the layout is compacted in layer order only when a newcomer does not fit or a
layer would stay unowned. Light ranges therefore follow layer ownership rather
than light order, and together cover every view once. Views index the
transmission arrays; the atlas square is independent of the view index. GPU culling tables size storage from the
current camera, directional, opaque local and transmitting local view count
and retain grown capacity. Opaque local views exclude refractive casters while
transmitting views select them; directional caster classification is unchanged.

Static local-shadow reuse is accepted per physical target image and atlas
square. The renderer supplies a retained-resource generation and the valid
atlas layers before selection. Face passes load the atlas and clear only their
own square, so a `Shadow.Local.Clear` pass clears the whole atlas first when it
has no retained contents, and every face then redraws. A selected light group is reusable only when every required
face has committed content for that image and its cached record matches the
resource generation, light identity and type, face index within the light, face
projection and atlas square, static-world generation, and publication
generation. Shadow
strength is receiver-side and never forces a redraw. Any dynamic caster whose
bounds overlap the light, an unavailable dynamic-bounds scan, or an active asset
publication forces the complete group to render. The dynamic scan is bounded by
`reuse_dynamic_scan_budget`; exceeding it forces rendering. Pending history is
published only after submission succeeds and is discarded when the frame is
cancelled or fails. Replacing the retained graph image clears its valid contents,
so the next selected group renders before it can be reused. Local shadows do not
use directional fit retention or SDSM.

On Metal, a reused face's opaque and transmission culling views reject every
candidate, skip their indirect-command reset and encoding, and get no frame
root; no pass executes them. The per-view command reset had cost about 0.8 ms
per frame for three cached point lights in the Bistro street view. Vulkan has no
per-view reset and still classifies reused views; that cost is unmeasured on
the development host.

Local transmitting shadows retain two ordered surface crossings at 512², capped
by the configured opaque-map extent. Each crossing stores D32 depth and RGBA16F
cumulative RGB transmission. A third depth-only crossing blocks receivers beyond
capacity. Receivers before a crossing do not inherit its attenuation. Each PCF
tap selects its depth-gated prefix, multiplies it by opaque visibility, then
contributes to the average. Point taps retain cross-face reprojection; volumetric
injection uses the same RGB visibility with its existing single tap.

The crossing coefficient uses authored transmission, base color, metallic,
layered GGX/sheen/clearcoat reflection loss and volume absorption along the light
ray. Material textures and cutout coverage are evaluated in the shadow raster;
a zero-transmission texel blocks light. Authored thickness supplies absorption
length after directional instance scaling. Rays remain straight and do not
produce refracted caustics. Thin-sheet diffuse transmission remains an opaque
shadow caster. This local-light model does not change directional shadows.

The five transmission arrays share the opaque pool's per-image graph owner,
retirement and submission lifecycle. A group refreshes all six resources
together. Reuse requires matching generations for all five transmission images
and valid contents for every required face. An incomplete or replaced prefix
pool forces a complete group redraw; cancelled work never promotes history.
Scenes with no refractive candidates allocate no transmission pool or views.

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

`tools/bake_reflection_probe.py` owns offline six-view capture. A bake records
the source scene manifest, per-face capture and report digests, output digest,
and one stable native provenance record in `<output>.bake.json`. It captures the
six KTX cube faces in `+X, -X, +Y, -Y, +Z, -Z` order with local probes disabled,
then the packer vertically flips each top-left capture exactly once. The emitted
`.vkt` is an uncompressed KTX2 cube with six RGBA16F faces, one array layer, and
one mip level. It has no runtime-generated source mips or orientation repair.

Example bake and freshness check:

```sh
python3 tools/bake_reflection_probe.py \
  --scene assets/scenes/example.json \
  --position 0 1 0 \
  --output assets/probes/example.vkt

python3 tools/bake_reflection_probe.py \
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
and, for every opaque pixel, stores each in-range shadowed light's filtered RGB
visibility, with strength applied, in that light's layer of an eight-layer
full-resolution RGBA8 array. Each light's layer travels in
`VkrLocalShadowView.shadow_params.y`. The pass repeats deferred lighting's
world-position reconstruction, light traversal, range and cone tests, and
back-lit normal choice, so lighting reads exactly the layers written for its
pixel. The clearcoat's second visibility query for a differing coat normal,
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
host. Probe bounds/ranges are not geometry visibility. The removed hard influence-AABB
experiment is not an occlusion mechanism. GTAO attenuates local indirect diffuse
only and does not establish arbitrary wall or furniture occlusion.

## Consequences

Lighting is bounded and independent of draw partitioning. Unshadowed lights and probe bounds can still leak illumination through geometry.
The D32 atlas uses 64 MiB per physical target image whatever the face count,
before culling and upload buffers. A selected local group redraws whenever its retained content
or reuse predicates are invalid; rendering cost scales with those groups and
caster overlap. The shadow mask adds 32 bytes per pixel while local shadows are
active, about 28 MiB per physical image at 1280x720. With refractive casters,
the full 16-face transmission pool adds
112 MiB per physical image, or 336 MiB across three images, excluding allocation
alignment and culling buffers. Each refreshed face adds two material/depth passes
and one overflow-depth pass. Static reuse avoids those raster passes while the
pool remains valid. These are storage and pass budgets, not a timing claim.

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

Scene scale exceeds these capacities, stored probe source requirements change,
or local-light transport requires more crossings, refracted caustics, or a
different memory or raster budget.

## Implementation

[`scene_loader.c`](../../runtime/src/renderer/resources/loaders/scene_loader.c)
parses and owns direct source loading. [`vkr_world_resources.c`](../../runtime/src/renderer/systems/vkr_world_resources.c)
requests the one-time probe bake. [`bake_reflection_probe.py`](../../tools/bake_reflection_probe.py)
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
