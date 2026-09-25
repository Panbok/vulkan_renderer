---
status: implemented
updated: 2026-09-25
authority: adr
---

# ADR-057: Analytic scene-linear height fog

## Status

Accepted and implemented.

## Context

The renderer needs a bounded atmospheric baseline that changes opaque, sky,
transmission, and blend radiance in scene-linear space. A final composited-image
fog pass would apply the opaque depth to layered transmission and fog already
fogged feedback a second time.

## Decision

Each scene owns optional `fog` JSON settings: `enabled`, RGB `color`, `density`,
`base_height`, `height_falloff`, `max_distance`, `sky_distance`, `sky_lighting`
and `anisotropy`. Color, density, and height falloff must be finite and
nonnegative. Base height must be finite. Both distances must be finite and
positive, and sky distance must not exceed maximum distance. Sky lighting lies
in [0, 1] and anisotropy in [-0.95, 0.95]. The runtime defaults to disabled fog
with scene-linear color (0.5, 0.6, 0.7), density 0.01, base height zero, height
falloff 0.1, both distances 10,000 world units, and zero sky lighting and
anisotropy.

Scene loading validates every supplied field before mutation, including a disabled
fog object. A zeroed disabled frame packet remains valid for older callers. The
standard runtime copies the scene record into FrameGlobals every frame. The editor
has no scene-environment panel or scene-level undo record, so JSON is the only
authoring route in this slice; editor integration is separate work.

The frame input carries `VkrFogSettings`. Frame preparation converts it to one
48-byte `VkrFogGpuParams` record per frame slot; disabled or zero-density fog
prepares an all-zero record, and preparation zeroes sky lighting unless the
frame publishes an enabled atmosphere. Metal stores the fog address at byte 504
of its frame root and Vulkan at byte 568. The fog compute roots are 176 bytes on
both backends: they also address the frame's sky record, the aerial volume and
a lit frame root that supplies the sun and sky light.

Fog is analytic height/distance attenuation toward an equilibrium in-scatter
radiance `J`. `J` is the authored scene-linear `color`, or, when sky lighting is
positive, the light a gray unshadowed medium scatters from the atmosphere:
`J = sky_lighting * (L_sky + E_sun * HG(g, cos theta))`. `L_sky` is the sky
light's average radiance, the published L2 SH mean scaled by the environment
intensity and diffuse controls and zero while the sky light is disabled. `E_sun`
is the frame's directional sun irradiance and `HG` the normalized
Henyey-Greenstein phase with the authored anisotropy `g`. `cos theta` is the
cosine between the view ray and the direction toward the sun, so positive `g`
scatters forward. `sky_lighting` acts as the single-scattering albedo. Fog
allocates no image, LUT, froxel grid, or fog history. The
opaque/sky compute pass runs after SSR composite and before the opaque transmission
pyramid. It reads and writes only the matching HDR pixel, preserving alpha. The
same pass applies [ADR-058](058-revision-baked-sky-atmosphere.md) aerial
perspective to opaque pixels before height fog, and runs when either is active.

Transmission fogs only the newly evaluated local lobes and keeps the ordered
feedback source already fogged. Its composition is
`T * local + (1 - T) * J * (1 - W) + already_fogged_background * W`,
where `W` is the current transmission-feedback coefficient and `J` is evaluated
along the layer's view ray. World blend fogs RGB
and retains source alpha. Canonical fog parameters participate in normal temporal and SSR content
signatures. Preparation also compares them with the last successfully submitted
fog record and requests a temporal scene-change reset before exposure
preparation. Failed or cancelled frames do not replace that record.

## Consequences

The baseline gives opaque geometry and sky a bounded aerial-perspective treatment
without retained fog resources. Transmission uses a straight screen-ray local-lobe
approximation; it does not integrate optical depth along a refracted exit ray.
Sky-lit in-scatter ignores sun shadowing and local lights, and uses the sky
light's average rather than its directional distribution. There are no shafts,
shadowed fog lights, spatially varying media, or editor controls in this
decision; [ADR-059](059-froxel-volumetric-fog.md) owns shadowed volumetric
scattering.

Native Vulkan execution is unavailable on the current macOS host. Both shader
implementations compile, but native bilateral parity remains unverified.

## Verification

Release and editor wrappers pass. Metal opaque/sky comparisons against an
independent optical-depth calculation cover 98,304 pixels each for constant
density, height falloff and distance caps; maximum HDR errors are 0.0002448,
0.0004891 and 0.0001433. Disabled fog preserves the previous full HDR payload
byte for byte. A zero-specular clear-glass fixture preserves fogged feedback
(maximum error 0.0007009); the BLEND source-lobe check passes at 0.0001494.

Metal API validation and the editor graph variant pass. Bistro captures contain
finite HDR pixels at 1280x720 and 640x360 internal resolution. Fog adds no graph
images or image bytes. A local, non-authoritative Release observation measured
the fog pass at 0.06967 ms at 1280x720 and 0.02349 ms at 640x360 (16 valid
samples each); these are diagnostic costs, not a performance baseline.
Vulkan SPIR-V validation/reflection confirms the 176-byte
compute root, 48-byte parameters and frame offsets
([fog-spirv.txt](../../assets/verification/renderer-features/fog-spirv.txt));
affected Vulkan host syntax checks pass. Native Vulkan execution and bilateral
image comparison remain unavailable.

Sky-lit in-scatter has a retained arithmetic regression,
`python3 tools/checks/check_froxel_regression.py`, which executes the shared
helpers through Slang CPU compilation. It checks the Henyey-Greenstein
normalization and mean cosine, the constant-colour fallback, the sky-lit
in-scatter and the SH average against a Fibonacci-sphere mean. On Bistro
(`local.fog.bistro.sky`, sky lighting 1, anisotropy 0.6) a far facade under
the constant colour, RGB (0.131, 0.173, 0.214), becomes (0.0725, 0.0877,
0.0882), and a view toward the sun shows the forward lobe. Metal API validation
of that case passes without diagnostics (report
sha256:8f2a50ef33728b05316e5cf2e296e6129bfb4933cd2e106518235aecffbc5e0f).

## Alternatives considered

A final-HDR fog pass cannot distinguish opaque depth from layered transmission.
Optical-depth feedback would model refracted paths more coherently but needs
additional full-resolution resources and declared feedback dependencies. A LUT
sky model needs a contract tying visible sky, sun illumination, and IBL together.
A froxel volume needs a selected-light, shadow, temporal-history, and 3D-resource
policy.

## Revisit when

Scenes need shadowed analytic in-scatter, light shafts, local volumes, refracted
exit-ray integration, or interactive scene-environment controls.

## Implementation

- [Public fog contract](../../renderer/src/vkr_fog.h)
- [Scene authoring and loading](../../runtime/src/renderer/systems/vkr_scene_system.h) and [scene loader](../../runtime/src/renderer/resources/loaders/scene_loader.c)
- [Frame input](../../renderer/src/vkr_frame_input.h) and [frame validation](../../renderer/src/vkr_frame_input.c)
- [Analytic shader kernel](../../renderer/src/shaders/shared/fog_kernel.slangh)
- [Metal fog root](../../renderer/src/metal/vkr_metal_packet_abi.h) and [Vulkan fog root](../../renderer/src/vulkan/vkr_vulkan_internal.h)
