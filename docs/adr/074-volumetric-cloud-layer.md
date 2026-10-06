---
status: implemented
updated: 2026-10-06
authority: adr
---

# ADR-074: Volumetric cloud layer

## Status

Accepted and implemented on both backends. Both pipeline classes run the
layer; the tiled pipeline composes it in `Tiled.Atmosphere`
([ADR-087](087-gpu-class-graphics-pipelines.md), decision 6). Each backend
records its own native evidence. On Metal, execution and the frame budget were
verified on the desktop implementation, removed on 2026-10-06, and ADR-087
records the tiled cloud trace and draw costs. On Vulkan, the cloud-lit sky light
(2026-10-05) runs and passes synchronization validation on the Windows host.
The Metal shader library, ABI reflection and the M1 budget for
`Clouds.SkyLight` were open gates when it landed; this ADR records no Metal
result for it.

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
atmosphere. A scene without the object has no clouds. The editor's
scene-creation job gives a new physical-sky scene `{"enabled": true}`, and
keeps an imported scene's authored atmosphere and cloud objects.

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
outside the map are unshadowed. The map follows the sky's key light, the light
that drives direct lighting ([ADR-081](081-physical-night-sky.md)). At night
that light is the moon, so the map, its centre and every sun term above use
the moon's ray instead.

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
march through the layer. A sample the planet hides from the sun skips that
march. While the moon is the key light, the moon lights the layer the same
way, with its phase-scaled irradiance and its own light march. The sun still
lights high cloud after it sets at the observer, which gives twilight colour. A forward (g = 0.8) and backward (g = -0.3, weight
0.3) Henyey-Greenstein mix scatters it over four multiple-scattering octaves
that scale light extinction by 0.25, scattering by 0.5 and eccentricity by 0.5
each. The published sky source's own L2 SH average, dimmed toward the layer
base, supplies ambient light; the sky record names that clear slot, so the
layer never lights itself with the cloud-lit sky light below. Steps integrate energy-conservingly with unit albedo. Aerial
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

In the desktop pipeline, deferred lighting composites sky pixels as
`L_sky * T + S` from the current history instance; the tiled pipeline's
`Tiled.Atmosphere` lays the same layer and disc over its sky pixels. `L_sky`
holds the sky and the sun glow; the sun disc is added
after it, scaled by `saturate((T - 0.1) / 0.9)`. A cloud that reads as solid
against the sky still passes a few percent of light, and the disc's radiance is
thousands of times the sky's, so the physical `T` would show it through that
cloud as a white point; the disc instead follows the cloud's apparent opacity.
The sun term of deferred, forward and transmission shading
multiplies by the cloud map; froxel injection multiplies the sun's visibility;
sky-lit analytic fog multiplies its sun irradiance by the map at the camera.
Every pass that evaluates the sun declares a read of `cloud_shadow` at binding
33, and deferred lighting reads `cloud_history` at binding 34.

`Clouds.SkyLight` puts the layer into the global sky light. For every texel
of the published sky source's 16-texel mip it marches the layer from the
camera along the texel's direction with 24 primary steps, a fixed mid-step
offset and no aerial perspective (the volume covers only the view), and
stores the in-scatter `S` and transmittance `T` in a frame-upload chain. `S`
is pre-exposed, so it is brought into the source's `2^radiance_stops` scale.
A second dispatch projects `L_clear * T + S` with the source's own deringing,
through the same projection loop as the revision bake, into a per-frame-slot
SH slot after the pool's slots (`VKR_SH_CLOUD_SLOT_FIRST`). A third averages
the chain down to one texel per face, one workgroup per face. Frame roots
prepared after the pass name the SH slot as the global sky light, and the sky
record names the chain, so diffuse IBL, the sky ambient of sky-lit and froxel
fog, reflection-probe blending and global prefiltered specular follow cloud
cover. Global specular samples the clear prefilter, reads the chain
bilinearly at the two levels whose texel size matches the prefilter's
roughness level and composites the same way. The pass reads and writes no
graph resource, enters the graph's first ready wave and ends with the barrier
the revision bake uses for its SH writes. Without a published source it
dispatches nothing, the clear slot stays global and the sky record names no
chain.

## Consequences

The sky, direct sunlight, fog, aerial perspective and diffuse sky light agree
about cloud cover, and the layer drifts with its wind. A surface in cloud
shadow takes ambient light from the cloud layer as the camera sees it, not
only from the clear sky, and sky reflections show the layer. The SH and the
chain are global: they follow the camera's sky, not cloud shadow at each
surface. The chain's 16-texel faces blur clouds in mirror-like reflections,
where the clear prefilter stays sharp. Reflection probes and baked diffuse
volumes, which bake offline, see a clear sky. A cloudless sky projects the
16-texel mip instead of the bake's 32-texel mip, which changes the SH
slightly. Deferred lighting pays for the chain lookup in global specular.

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
pixel, absorbs. Coupling clouds into the revision bake would freeze a
snapshot that the wind and camera make stale, and each rebake creates four
textures; the per-frame 16-texel composite follows both for about 0.09 ms.
Scaling the clear SH by coverage was declined: it has neither the clouds'
direction nor their colour. Reflections read the chain from a buffer rather
than a prefiltered cloud cube: a cube texture would need per-slot images and
a prefilter pass, while the chain shares the frame-upload lifetime and its
box-filtered levels stand in for the GGX lobe at the cloud's resolution.

## Revisit when

Mirror-like reflections need sharp clouds, reflection probes need clouds,
surfaces need local cloud-shadowed ambient, or scenes need multiple layers or
cloud types, fly-through or above-cloud cameras, or a lower budget. Revisit the
multiple-scattering model if front-lit clouds need calibrated brightness.

## Implementation

- [Settings, validation and preparation](../../renderer/src/vkr_clouds.h), [sky record](../../renderer/src/vkr_atmosphere.h) and [frame input](../../renderer/src/vkr_frame_input.h)
- [Scene loading](../../runtime/src/renderer/resources/loaders/scene_loader.c), [revision state](../../runtime/src/renderer/systems/vkr_scene_system.h) and [wind clock](../../runtime/src/application/vkr_standard_scene_runtime.c)
- [Shared kernel](../../renderer/src/shaders/shared/cloud_kernel.slangh)
- [Metal kernels](../../renderer/src/shaders/metal/msl/ibl/clouds.metal) and [Vulkan kernels](../../renderer/src/shaders/vulkan/slang/ibl/clouds.slang)
- [Authored graph](../../assets/render_graphs/main.rendergraph.json)
- Cloud-lit sky light: [Vulkan preparation](../../renderer/src/vulkan/vkr_vulkan_deferred.c), [Metal preparation](../../renderer/src/metal/internal/vkr_metal_packet_frame.inc), the shared projection loops in [Vulkan](../../renderer/src/shaders/vulkan/slang/ibl/default.slang) and [Metal](../../renderer/src/shaders/metal/msl/ibl/sh_projection.metal), and the [SH slot layout](../../renderer/src/vkr_ibl_math.h)

## Verification

Cloud-lit sky light, Vulkan Release on the Windows host (RX 6700 XT,
1280x720), a scratch Bistro case looking down on the rooftops under the
default layer (`clouds_bistro_local.scene.json`, coverage 0.5) with manual
exposure: a roof in cloud shadow moved from sRGB (17, 31, 43) without the pass
to (30, 39, 44) with it, and the visible sky stayed identical. One
non-authoritative 16-frame process measured `Clouds.SkyLight` at 0.084 ms
p50 (0.103 ms p95) beside a 0.199 ms `Clouds.Trace`; marching the 32-texel mip
with 48 steps cost 0.22 ms for the same roof value. Debug Vulkan with
`VK_LAYER_KHRONOS_validation` and synchronization validation reported no
error for that case.

Cloud-lit reflections, the same host and scene from the street-level camera
of `bistro_bright_spot_snapshot` with SSR on: window glass that reflected
blue sky reflects the grey layer overhead (a window region from sRGB
(21, 25, 25) to (23, 26, 26), maximum pixel change 36 of 255, 2% of pixels
above 4), and sky pixels are unchanged. Two non-authoritative 16-frame
processes with and without the chain lookup measured
`Lighting.Deferred.Fullscreen` at 1.473 and 1.428 ms p50 and
`Clouds.SkyLight` at 0.089 and 0.091 ms. Debug synchronization validation of
that case reported no error.

The CPU suite checks the shadow-map centre against the sun ray's entry point
into the base, the wind wrap, the render-mode rule and the authoring domain,
and loads and rejects scene `clouds` objects. When the layer shipped, the full
main graph had 167 passes, then the renderer's pass capacity.

On the Metal desktop implementation, removed on 2026-10-06 (Release, M1 Pro
development host, 1280x720), five non-authoritative processes of 300 measured
frames each measured the layer in `bistro.scene.json` (coverage 0.45):

| View | Clouds.Trace | Clouds.Shadow | Deferred lighting change | GPU pass sum change |
|---|---|---|---|---|
| Sky, pitch 25 degrees | 0.8697 ms | 0.1079 ms | +0.0692 ms | +1.0398 ms |
| Street | 0.1868 ms | 0.1086 ms | +0.1947 ms | +0.4670 ms |
| Zenith | 0.7690 ms | 0.1080 ms | n/a | n/a |

The worst view spends about 1.05 ms, inside the 1.5 ms budget. The before
reports (sha256:535fa6e0 sky, 6f844870 street) and after reports
(sha256:ba20c628 sky, da8ff653 street, 278f2467 zenith) share the case and
profile; the dirty tree keeps them non-authoritative.

On the same implementation, a cloud-free Bistro sky kept geometry HDR
byte-identical; 33,178 sky pixels moved by compiler codegen, 98% by one
half-float step. Vulkan SPIR-V validation and layout reflection pass for the
five cloud modules and every sky-record consumer
([clouds-spirv.txt](../../assets/verification/renderer-features/clouds-spirv.txt)).
Native Vulkan evidence is limited to the cloud-lit sky light runs above.