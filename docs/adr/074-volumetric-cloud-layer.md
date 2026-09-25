---
status: implemented
updated: 2026-09-25
authority: adr
---

# ADR-074: Volumetric cloud layer

## Status

Accepted and implemented on both backends. Metal execution, API validation and
the frame budget are verified; native Vulkan execution and bilateral image
comparison remain unavailable on the development host, so the shader contract
is **UNALIGNED** under [ADR-044](044-shader-cross-backend-contract.md).

## Context

[ADR-058](058-revision-baked-sky-atmosphere.md) made the atmosphere the only
sky and gave it a camera-dependent sky-view lookup and aerial-perspective
volume. A clear sky cannot occlude the sun, cast moving shadows or show weather.
The owner approved a single volumetric layer with its own decision and a frame
budget of at most 1.5 ms Metal Release at 1280x720 on the M1 development host.

## Decision

### Authoring and publication

Scenes may author an optional `clouds` object: `enabled`, `base_altitude_m`,
`top_altitude_m`, `coverage`, `density` and `wind_mps`. A present object
defaults to enabled with a base at 1,500 m, a top at 4,000 m, coverage 0.5,
extinction 0.02 per metre at unit noise density and a 10 m/s wind along world
X. The base is nonnegative, the top at most 20,000 m and at least 100 m above
the base, coverage and density lie in [0, 1], and each wind component is finite
with magnitude at most 200 m/s. The loader validates every supplied field even
when the layer is disabled, and rejects an enabled layer without an enabled
atmosphere.

The layer is part of the atmosphere revision: the runtime's candidate carries it
and publication moves it into the active settings with the lookup textures.
The runtime advances a float64 wind offset by the published wind every frame and
wraps it at the 32 km weather period, where every noise texture tiles.
Frame-input version 50 carries the published settings and the offset in the
sky payload; validation requires the offset in [0, 32,000) metres and an enabled
atmosphere for an enabled layer.

### Prepared parameters

Frame preparation appends a 64-byte `VkrCloudGpuParams` record to the sky
parameters, which grow to 352 bytes. It holds the base and top radii from the
planet centre, extinction per kilometre, coverage, the reciprocal noise periods
(8 km base, 1 km detail, 32 km weather), the wrapped wind offset, the 60 km
march distance, and the sun-projected shadow map's centre and extent. Clouds
render under the aerial-perspective rule: default render mode and a
perspective camera. Otherwise the record is zero and every consumer takes the
clear-sky path. The frame flag `clouds_enabled` gates both cloud resources and
passes.

### Noise

The renderer owns three R8 noise textures and generates them once on the GPU:
a 128^3 base volume (Perlin noise dilated by low-frequency Worley noise, shaped
by higher-frequency Worley noise), a 32^3 detail volume (three Worley octaves)
and a 256^2 weather map. The combined base shape spans roughly [0.71, 0.93]
between its 1st and 99th percentiles and the weather map [0.35, 0.70] between
its 5th and 95th; generation stretches both to [0, 1] so coverage selects any
fraction of the layer. The three textures total 2.1 MiB for the renderer's
lifetime. Metal generates them in a startup submission and waits for it,
because Metal 4 does not order later command buffers after it. Vulkan
publishes them in permanent descriptor rows (sampled rows 12-14, storage rows
1-3, sampler row 4 with repeat addressing) and records the generation in the
first submitted IBL bake; a cancelled frame leaves it pending.

### Passes

`Clouds.Shadow` writes a 512^2 R16F per-frame-slot map: the transmittance of
the whole layer along the sun ray that enters its base at each texel. The map
covers 8 km around the point where the camera's sun ray enters the base,
snapped to its 15.6 m texels. The planar march takes 16 climbing steps through
the base shape without detail erosion and holds the sun's elevation sine at
0.1 or more. Surfaces below the layer sample the map where their own sun ray
enters the base; surfaces above the base use their own column, and surfaces
outside the map are unshadowed.

`Clouds.Trace` marches a half-resolution history instance. A texel traces when
any full-resolution pixel within one pixel of its 2x2 footprint shows sky, so
every bilinear tap that composition reads is traced; other texels write an
untraced marker (alpha 2). Rays use canonical clip coordinates, which equal
screen coordinates on both backends, with an eight-frame Halton sub-texel
jitter and a per-frame interleaved-gradient march offset. Each ray takes 48
steps over its segment through the spherical layer, clipped at 60 km with a
fade over the last third, and stops below 1% transmittance.

At each sample the base shape, weather and coverage set the density, and the
detail volume erodes its edges. Sunlight is the top-of-atmosphere irradiance
times the atmosphere's transmittance lookup and a five-step doubling light
march through the layer. A forward (g = 0.8) and backward (g = -0.3, weight
0.3) Henyey-Greenstein mix scatters it over four multiple-scattering octaves
that scale light extinction by 0.25, scattering by 0.5 and eccentricity by 0.5
each. The sky light's L2 SH average, dimmed toward the layer base, supplies
ambient light. Steps integrate energy-conservingly with unit albedo. Aerial
perspective applies at the transmittance-weighted depth of the removed light
in the over-feedback form `S * T_ap + S_ap * (1 - T_cloud)`, so the composite
restores the atmosphere in front of the cloud.

The trace blends 10% of the current sample into history reprojected by
direction through the producer's canonical view-projection. Clouds lie
kilometres away, so camera translation parallax is ignored. History requires
the same traced layer (layer, noise, march distance and world scale), grid and
graph generation, a producer inside the ring, no temporal reset, and no
untraced marker among the bilinear taps. With temporal antialiasing only its
exact predecessor is eligible, even in flight: the submission already orders
it, and accumulation never depends on GPU timing. Without temporal
antialiasing the newest completed producer seeds the trace. The harness
resets the wind clock with its other phase clocks, so bootstrap duration does
not move the layer in captures.

Deferred lighting composites sky pixels as `L_sky * T + S` from the current
history instance. The sun term of deferred, forward and transmission shading
multiplies by the cloud map; froxel injection multiplies the sun's visibility;
sky-lit analytic fog multiplies its sun irradiance by the map at the camera.
Every pass that evaluates the sun declares a read of `cloud_shadow` at binding
33, and deferred lighting reads `cloud_history` at binding 34.

## Consequences

The sky, direct sunlight, fog and aerial perspective agree about cloud cover,
and the layer drifts with its wind. The global sky light stays the clear-sky
atmosphere: the revision bake does not include clouds, so image-based ambient
and specular reflections show a clear sky, and a cloud that covers the sun
does not dim the SH. Baked diffuse volumes likewise see a clear sky.

The four-octave approximation underestimates multiple scattering in optically
thick clouds, so front-lit faces render darker than a Lambertian reflector of
the same albedo. The planar shadow projection and single sun column do not
model surfaces inside or above the layer. Half-resolution tracing with
bilinear composition softens cloud edges, and one-pixel geometry silhouettes
against clouds can lag for a few frames after disocclusion.

## Alternatives considered

Cooked noise assets would need a cooker, a Bakery job and 3D texture support
in the texture system; GPU synthesis gives the same fixed textures without an
asset. Full-resolution tracing would march four times the rays of the
measured 0.87 ms half-resolution pass, and dropping history would need more
steps per frame for the same noise. A separate full-resolution composition pass
would add work that the deferred sky branch, which already writes every sky
pixel, absorbs. Coupling clouds into the revision bake needs a cloud ambient
model that does not depend on the SH it produces, and matching Vulkan and CPU
baker transport; it is deferred rather than approximated.

## Revisit when

Scenes need clouds in image-based lighting or reflections, multiple layers or
cloud types, fly-through or above-cloud cameras, or a lower budget. Revisit the
multiple-scattering model if front-lit clouds need calibrated brightness.

## Implementation

- [Settings, validation and preparation](../../renderer/src/vkr_clouds.h), [sky record](../../renderer/src/vkr_atmosphere.h) and [frame input](../../renderer/src/vkr_frame_input.h)
- [Scene loading](../../runtime/src/renderer/resources/loaders/scene_loader.c), [revision state](../../runtime/src/renderer/systems/vkr_scene_system.h) and [wind clock](../../runtime/src/application/vkr_standard_scene_runtime.c)
- [Shared kernel](../../renderer/src/shaders/shared/cloud_kernel.slangh)
- [Metal kernels](../../renderer/src/shaders/metal/msl/ibl/clouds.metal) and [Vulkan kernels](../../renderer/src/shaders/vulkan/slang/ibl/clouds.slang)
- [Authored graph](../../assets/render_graphs/main.rendergraph.json)

## Verification

The CPU suite checks the shadow-map centre against the sun ray's entry point
into the base, the wind wrap, the render-mode rule and the authoring domain,
and loads and rejects scene `clouds` objects. The full main graph has 167
passes, the renderer's pass capacity.

Metal Release on the M1 Pro development host at 1280x720, five
non-authoritative processes of 300 measured frames each, measured the layer in
`bistro.scene.json` (coverage 0.45):

| View | Clouds.Trace | Clouds.Shadow | Deferred lighting change | GPU pass sum change |
|---|---|---|---|---|
| Sky, pitch 25 degrees | 0.8697 ms | 0.1079 ms | +0.0692 ms | +1.0398 ms |
| Street | 0.1868 ms | 0.1086 ms | +0.1947 ms | +0.4670 ms |
| Zenith | 0.7690 ms | 0.1080 ms | n/a | n/a |

The worst view spends about 1.05 ms, inside the 1.5 ms budget. The before
reports (sha256:535fa6e0 sky, 6f844870 street) and after reports
(sha256:ba20c628 sky, da8ff653 street, 278f2467 zenith) share the case and
profile; the dirty tree keeps them non-authoritative.

The Bistro Metal text baseline was re-accepted with the layer (generation
`sha256:74b6e5c517c668ed17354a0d3f1026bc0d0767b82b17666727fb9c1ca3f4c19c`);
two same-build reruns compare against it with failed-pixel ratio 0 and mean
absolute error at most 8.7e-7 per view. Its successor for the directional sun
light of ADR-058 keeps the same look.

Metal API validation of `local.clouds.bistro_sky` passes without diagnostics
(report sha256:dee8faa8e4cc6d7a6a458fcc69e0d8776b30d2263327995517e0c60266324f8a).
A cloud-free Bistro sky keeps geometry HDR byte-identical; 33,178 sky pixels
move by compiler codegen, 98% by one half-float step. Vulkan SPIR-V validation
and layout reflection pass for the five cloud modules and every sky-record
consumer
([clouds-spirv.txt](../../assets/verification/renderer-features/clouds-spirv.txt)).
Native Vulkan execution and bilateral comparison remain unavailable.
