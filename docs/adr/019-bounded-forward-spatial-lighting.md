---
status: implemented
updated: 2026-09-07
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
bounded depth-array pool when `casts_shadow` is set. glTF imports enable it by
default for finite positive ranges and supported spot angles. Unlimited-range
lights and spots with a 90-degree outer half-angle remain unshadowed, preserving
valid glTF imports without inventing a range or changing the authored cone.
Saved editor overrides can disable shadows; scene-authored JSON lights remain
opt-in. The default budget is 16 faces at
1024 squared: a spot uses one perspective view and a point uses six in
+X, -X, +Y, -Y, +Z, -Z order. Stable scene-light order allocates complete light
groups; requests that do not fit remain unshadowed. The budget and map size can
be reduced through `VkrShadowConfig`. Shadowed lights require a finite positive
range, and shadowed spot outer half angles must be below 90 degrees.

Application preparation owns the fixed frame-local view payload. Each physical
target image owns its local depth array through the existing graph resource and
GPU-completion lifecycle. Local-shadow selection remains bounded by the configured
face budget: a spot owns one view and a point owns six contiguous views. Graph
reads name only the selected layers. GPU culling tables size storage from the
current camera, directional and local view count and retain grown capacity.

Static local-shadow reuse is accepted per physical target image and depth-array
layer. The renderer supplies a retained-resource generation and valid-layer mask
before selection. A selected light group is reusable only when every required
face has committed content for that image and its cached record matches the
resource generation, selection layout, light identity and type, face projection,
static-world generation, and publication generation. Any dynamic caster whose
bounds overlap the light, an unavailable dynamic-bounds scan, or an active asset
publication forces the complete group to render. The dynamic scan is bounded by
`reuse_dynamic_scan_budget`; exceeding it forces rendering. Pending history is
published only after submission succeeds and is discarded when the frame is
cancelled or fails. Replacing the retained graph image clears its valid contents,
so the next selected group renders before it can be reused. Local shadows do not
use directional fit retention or SDSM.

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

Receivers use nine comparison-PCF Poisson taps with a 1.5-texel radius. Normal
offset is two local shadow texels and receiver bias is one texel, converted using
the perspective footprint at receiver depth. Point taps reconstruct rays and
reproject into adjacent faces. Both deferred and transparent lighting consume
the same local visibility semantics. Probe bounds/ranges are not geometry visibility. The removed hard influence-AABB
experiment is not an occlusion mechanism. GTAO attenuates local indirect diffuse
only and does not establish arbitrary wall or furniture occlusion.

## Consequences

Lighting is bounded and independent of draw partitioning. Unshadowed lights and probe bounds can still leak illumination through geometry.
A full 16-face D32 pool uses 64 MiB per physical target image, before culling
and upload buffers. A selected local group redraws whenever its retained content
or reuse predicates are invalid; rendering cost scales with those groups and
caster overlap. There is no accepted timing claim.

Saved probes consume a source cubemap plus a runtime prefilter cubemap and SH
slot. Their offline artifact is valid only with its matching sidecar provenance;
`--check` verifies its output digest, scene-manifest source digests, face list,
and recorded provenance consistency. The saved source must remain a finite,
uncompressed RGBA16F KTX2 cube with the baker's face order and one-mip storage
contract.

## Alternatives considered

A first-N global list makes visibility depend on source order. Per-draw probe
selection fails on large meshes. Hard light influence boxes produced discontinuous
slabs without solving diffuse occlusion.

## Revisit when

Scene scale exceeds these capacities, stored probe source requirements change,
or geometric local-light visibility is required beyond the accepted local-shadow
pool.

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
