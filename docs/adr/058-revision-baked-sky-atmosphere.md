---
status: implemented
updated: 2026-09-25
authority: adr
---

# ADR-058: Revision-baked sky atmosphere, sky light and unified sun

## Status

Accepted. Runtime publication, sky-light controls, the constant fixture source,
the camera-dependent sky, aerial perspective, the per-frame directional sun
light, the sun glow and offline diffuse transport are implemented. Image skies are removed. Native
Vulkan execution remains unavailable on the development host.

## Context

Outdoor sky, global indirect lighting and directional sunlight must describe the
same atmosphere. Continuously re-baking global lighting as the camera moves
would change the resource and frame budgets beyond the accepted first stage.
The visible sky and distant surfaces, however, must follow the camera: one
observer-altitude cube cannot show aerial perspective or a sharp sun.
Equirectangular and cubemap image skies were a second global-environment
producer that could disagree with the sun, and they carried their own
conversion kernels, decoder and fallback policy.

## Decision

The atmosphere is the only sky. A scene's global environment tuple is a source
cube, its GGX prefilter, its L2 SH coefficients and, for an atmosphere, the
generation's two lookup textures. An enabled atmosphere produces it. Isolated
fixtures and previews may instead author a uniform `environment.constant`
radiance in `[0, 65504]`. The runtime uploads that value as a
one-texel-per-face RGBA16F cube and prepares it with the ordinary IBL bake.
Equirectangular and cubemap global environments, their conversion kernels, the
Radiance texture decoder and the cross-scene fallback environment are removed.
Local reflection probes keep their separately prepared cubemap sources.

The `environment` block is the sky light. Its `enabled`, `intensity`,
`diffuse_intensity`, `specular_intensity` and `sh_deringing` fields apply to
whichever source publishes the tuple. An atmosphere revision swaps only the
texture tuple, so authored and runtime-adjusted controls survive it. A disabled
sky light keeps the atmosphere sky visible and removes only its image-based
lighting. Without an environment block, an enabled atmosphere lights with unit
controls. A constant conflicts with an enabled atmosphere. The loader rejects
stale `equirect`, `cubemap` and `atmosphere.sh_deringing` fields with a message
naming the replacement.

Specular lighting samples the prefiltered cube through its native backend
resource. Source dimensions and prefilter mip count remain owned by the prepared
resource and shader contract. Diffuse response uses L2 SH under
[ADR-038](038-sh-l2-diffuse-irradiance.md). IBL preparation remains
backend-owned, explicitly synchronized work outside complete graph resource
declarations.

### Revision bake

The atmosphere bakes the global environment at an authored observer altitude
when atmosphere or sun settings change. Camera motion does not request another
bake. The drawn sky, disc, direct light, shadows, fog and clouds follow the
sun every frame: each frame pairs the published medium, which its lookups
baked, with the scene's current sun. Only the sky light, the source cube,
prefilter and SH, waits for a revision. A sun that differs from the latest
request queues one immediately while nothing is published or that request has
not started baking, since replacing it costs nothing; otherwise once 0.25
seconds have passed since the last, so a moving sun refreshes the sky light
at most four times a second and its final position always gets its own bake.
A request that arrives while a candidate bakes waits for it: the candidate
publishes, then the newest request bakes. Each generation owns a 256×64
transmittance and a 32×32 multiple-scattering RGBA16F lookup texture, 136 KiB
together. The runtime
creates them beside the candidate source and prefilter, and they publish and
retire with that tuple, so the visible sky keeps reading its own generation's
lookups while a candidate bakes. The bake uses 40 samples per transmittance
texel, 64 directions with 20 samples per multiple-scattering texel, and 32
samples per source texel on a 256-pixel-face source cube.

Source RGB contains atmospheric in-scatter. Rays that reach the planet add the
sunlit Lambertian ground, `solar × T_sun × ground_albedo × max(N·L, 0) / π`,
through the path transmittance; this is the first-order ground term the
multiple-scattering lookup already uses, so the lower hemisphere is lit ground
rather than black. The source excludes the sun disc and its alpha is unused, so
direct sunlight does not also enter the global indirect lighting.

One sun direction, angular diameter and attenuated RGB irradiance drive the
sky, directional lighting, cascaded shadows and contact hardening. A
directional light whose `atmosphere_sun` flag is set drives that sun, as UE5's
atmosphere sun light does. The flag defaults to true for lights in scene files
and to false for lights imported from glTF, so an asset's own sun cannot take
the sky; Bistro's glTF sun, for example, has zero intensity. Among enabled
flagged lights the renderer's rule applies, the lowest render id else the first
found, and the loader warns when a scene authors more than one. The light's
local rotation applied to `direction_local` points away from the sun. Its
colour, tinted by an optional `temperature_kelvin` in `[1000, 40000]` through
the blackbody helper below, times its intensity becomes the top-of-atmosphere
irradiance. A diameter in `(0, 5]` degrees replaces the visible disc; zero, a
hard-shadow light, keeps the authored disc. Without a flagged light the
authored `sun_direction` and irradiance apply. The scene resolves its sun light
once per frame, memoizing the temperature tint. While the atmosphere is
enabled, lighting sync ignores every entity directional light and the frame's
sun lights it. Before the first publication the scene has no sun.

Authors may give the top-of-atmosphere sun as `solar_irradiance` or through
`sun_temperature_kelvin` in `[1000, 40000]` and a scene-linear
`sun_illuminance`. The loader and offline baker resolve those through one
helper: Planck's law integrated against the Wyman-Sloan-Shirley fit of the CIE
1931 colour matching functions, converted to linear Rec.709, gamut-clamped and
normalized to unit luminance, then scaled by the illuminance. A missing
temperature keeps the default colour; a missing illuminance keeps the default
luminance. `solar_irradiance` conflicts with either field. Direct lighting
uses the observer irradiance: the top-of-atmosphere irradiance times the
40-sample transmittance integral from the observer altitude toward the sun,
the integral every lookup texel holds, or zero once the planet blocks the sun.
The runtime evaluates it on the CPU when the frame's sun or published medium
changes, and the offline baker uses the same code.
Authored top-of-atmosphere irradiance is uniformly scaled when necessary to
keep peak disc radiance, irradiance over the projected solid angle
`π sin²(angular radius)`, at most 60000, preserving RGB ratios and RGBA16F
headroom. Source and sky-view radiance are uniformly limited to 5000 so their
sum with the visible disc also fits RGBA16F. These are scene-linear calibration
values, not lux. The supported Earth model accepts density multipliers in
`[0,100]`, solar diameters in `[1e-16,5]` degrees, observer altitudes in
`[0,100000]` metres, Mie anisotropy in `[-0.95,0.95]`, a sun glow in `[0,100]`
and a world scale in `[0.001,1000]` metres per world unit. The input boundary rejects values outside
this numerical domain.

A revision prepares distinct candidate resources while the prior generation
remains active. Submission does not establish readiness. The scene publishes
source, prefilter, SH and lookups together only after native completion
confirms the whole candidate. Failure retains the previous generation.
Ordinary readiness queries do not wait. On Metal each upload is a separate
command buffer, and Metal 4 does not order them, so
the IBL bake waits on prior queue dispatch and blit work before reading its
source. Vulkan records the bake in the frame command buffer and makes each
lookup write visible to later compute sampling in submission order. The SH
pool's two environment entries cover the active tuple and its replacement
candidate.

### Camera-dependent sky

Frame input version 50 carries a sky payload: the published generation's
medium lit by the scene's current sun and its lookup textures, or the constant
radiance, and the published cloud layer of
[ADR-074](074-volumetric-cloud-layer.md). The renderer prepares a
352-byte sky record per frame slot, and every native frame root addresses it.
The camera stands on the planet below its world position, at the observer
altitude plus its world height times `atmosphere.metres_per_world_unit`,
clamped to the altitude domain. The scale defaults to one and does not
participate in the bake.

Every frame with a published atmosphere builds two graph-declared transient
images from the generation's lookups. A 192×108 RGBA16F sky-view lookup follows
Hillaire's 2020 non-linear parameterization around the camera: V spans zenith
to horizon and horizon to nadir, each squared toward the horizon, and U is
`sqrt((1 − cos φ) / 2)` of the azimuth φ from the sun. It stores the same
32-sample integral as the source cube, including the sunlit ground below the
horizon. A 32×32×32 RGBA16F aerial-perspective volume stores in-scatter and
mean transmittance along camera froxels at Hillaire's squared slice depths of
4 km per slice; one thread marches each column with two segments per slice.
Below the first texel's depth, aerial perspective fades linearly to identity at
the camera.

The deferred background samples the sky-view lookup and adds an analytic disc:
Neckel and Labs' power-law limb darkening with exponents
(0.397, 0.503, 0.652), normalized by `1 + a/2` so the disc integrates to the
published irradiance, attenuated by the transmittance lookup along the view ray
and uniformly limited to a 60000 peak. The disc is point-sampled; temporal
jitter resolves its edge. A physical 0.53-degree disc covers a few pixels and
reads as a star, so a glow surrounds it: veiling radiance `sun_glow * E /
theta^2` with theta in degrees after Stiles and Holladay's glare formula,
whose human-eye coefficient is about 10, faded smoothly to zero 25 degrees
out, where the atmosphere's own forward scattering already brightens the sky.
E is the same view-attenuated irradiance as the disc's, so the glow reddens at
sunset and vanishes once the sun sets; the peak is limited to 500 so sky, disc
and glow fit RGBA16F together. The atmosphere block authors `sun_glow`,
default 2, and zero turns it off. The glow changes only the drawn sky, never
lighting, the bake or its recipe hash. Sky pixels take no aerial perspective
because the lookup already integrates to the top of the atmosphere.

Aerial perspective is the farther medium. The in-place opaque fog pass applies
it before [ADR-057](057-analytic-height-fog.md) height fog and runs when
either is active; froxel application applies it before froxel fog.
Transmission applies it to new local lobes over already-attenuated feedback,
and world blend to its RGB, with the ADR-057 composition. Like analytic fog it
applies only in the default render mode with a perspective camera.

### Offline transport

The offline diffuse baker prepares the same lookup model and an immutable
scene-owned RGB cubemap before tracing. It derives one attenuated sun from the
first enabled atmosphere sun light, the runtime's choice for a scene with one,
suppresses every other directional light, including those imported with
meshes, and retains local lights. Its medium, transmittance and observer
irradiance come from the runtime's shared C atmosphere module. It applies the
same sky-light rules as the runtime. Sky radiance entering transport is scaled
by the sky-light intensity, the convention the removed image environment used,
and a disabled sky light removes it. Glass, multibounce paths and photon
caustics consume that lighting through the existing transport integrator. Bake
metadata records the model version, parameter hash and the SH window applied to
the atmosphere. Model version 2 adds the sunlit ground, so version-1 atmosphere
bakes report stale. Existing version-one sidecars without atmosphere provenance
remain usable for scenes that do not enable atmosphere. The baker accepts the
world scale but, like the runtime bake, does not use it.

## Consequences

Stable settings add only the per-frame sky-view lookup, aerial-perspective
volume and aerial application to ordinary frames; revisions pay the full sky
and IBL bake and temporarily retain both generations. A moving sun costs at
most one such bake every 0.25 seconds, and its sky light, which shapes
ambient light and reflections, trails the drawn sun by up to that interval
plus one bake. Each frame slot holds a
162 KiB sky-view lookup and a 256 KiB aerial volume. Global lighting and the
direct sun stay at the authored altitude while the visible sky and aerial
perspective follow the camera. Aerial perspective uses a gray transmittance,
and its froxels are coarse: within a few hundred metres at unit scale it is a
fraction of a percent of surface radiance. This stage does not provide local
participating media or volumetric light shafts; analytic fog remains governed
by [ADR-057](057-analytic-height-fog.md). Clouds occlude the visible sky and
shadow the sun, but the revision bake stays a clear sky.

Every scene that used an image sky now authors the atmosphere, or a constant
source when it had no sun or used a uniform furnace image. Bistro and its
derived fixtures use unit sky light: their former factors calibrated the much
brighter image sky, and kept values left shade unphysically dark. The two
fixtures that isolate IBL terms keep their zeroed factors. The physical sky is
roughly four stops dimmer in scene-linear terms than the removed image, so the
Bistro text snapshots set manual exposure 4.0, near the automatic meter's
value for the shaded square. Scenes without an environment block and without an
atmosphere render without a global environment instead of inheriting an earlier
scene's. The editor's scene-creation form offers a physical-sky toggle instead
of an HDR picker. Its material preview lights the canonical sphere with a
constant ambient and two rectangle-light softboxes.

## Alternatives considered

A directional light that shines independently of the atmosphere can disagree
with the sky and shadows. Drawing the published sun until a revision bakes
froze the sky and direct light while a light moved; UE5 likewise updates its
sky atmosphere every frame and recaptures the sky light separately. A GPU
query of the observer irradiance per frame would lag the direction by the
readback latency, whereas the CPU integral is exact and immediate.
Time-slicing the sky-light bake across frames, as UE5's real-time capture
does, would spread its cost but needs a multi-frame candidate protocol. A
larger visible disc would also widen the contact-hardening penumbra that the
same diameter drives; a flare that starts at the disc's radiance, as HDRP's
does, saturates to a hard-edged disc of nearly its full size at daylight
exposure, whereas a 1/theta^2 glow falls over orders of magnitude, so the
display transform shows a soft halo. Letting any enabled directional light drive the sky
handed Bistro's zero-intensity glTF sun the sky whenever the scene authored no
other light. Cancelling a baking candidate for each newer request starved
publication while a light moved.
Including the disc in both IBL RGB and direct lighting counts its energy twice.
Per-frame global-lighting integration and higher-resolution cubemaps exceed
this stage's accepted update and storage scope. Sampling the observer-altitude
source cube for the visible sky cannot follow the camera, and its 256-texel
faces limited the disc to a blocky coverage estimate. Renderer-owned lookup
caches let a candidate bake overwrite the lookups the published sky samples.
Applying aerial perspective in deferred lighting would skip SSR, SSGI and
subsurface radiance added afterwards. Keeping image skies as a second source
would retain two global-environment producers and their adapters; the constant
source covers the uniform furnace fixtures that no atmosphere can produce.
Direct equirectangular runtime sampling complicates filtering at poles and
seams.

## Revisit when

Clouds in global lighting, higher-resolution solar reflections or
camera-dependent global lighting require a different update or storage budget.

## Implementation

[Public settings](../../renderer/src/vkr_atmosphere.h),
[shared atmosphere math](../../renderer/src/shaders/shared/atmosphere_kernel.slangh),
[publisher contract](../../renderer/src/vkr_asset_publisher.h),
[frame input](../../renderer/src/vkr_frame_input.h),
[scene loader](../../runtime/src/renderer/resources/loaders/scene_loader.c)
and [environment preparation](../../runtime/src/renderer/systems/vkr_world_resources.c).
The [CPU atmosphere baker](../../tools/bake/vkr_bake_atmosphere.cpp) owns its
temporary LUTs and scene-lifetime source cube.

Metal Release captures cover zero extinction, day, sunset, elevated observer,
disabled fallback and Bistro. A focused native API-validation fixture covers
completion, superseded candidates, failure, disable and reload. A CPU/GPU
day-sky comparison outside two degrees of the sun has maximum RGB absolute
error 0.000853. A glass-ceiling smoke bake produces 27 valid probes, eight
interpolation cells and 9,072 caustic deposits from 20,000 photons. These
checks establish the available native path and bounded transport fixture;
SPIR-V compilation and host checks do not establish native Vulkan parity.

The atmosphere-only change passes `./build_release.sh`, `./build_editor.sh
Release`, the diffuse-baker build and `./build_test.sh`. The CPU suite adds
loader tests for rejected image fields, constant parsing and bounds, sky-light
controls under the atmosphere, conflicts and the moved SH window, plus Radiance
texture rejection. `tools/checks/check_editor_project_jobs.py` and
`tools/checks/check_path_contract.py` pass. Metal Release captures use
`tools/profiles/local-offscreen.json` with validation unset, except the furnace,
which uses `local-brdf-display-validation.json`:

| Observation | Result |
|---|---|
| Clearcoat furnace, constant source, `check_clearcoat_furnace.py` | pass; 36 samples, maximum error 0.00049 |
| `atmosphere_bistro_local` scene-linear luminance | zenith sky 0.0336, sunlit facade 0.122, shaded street 0.0215 |
| Bistro automatic exposure (`mac_bistro_exposure_parity_gtao_off`) | multiplier 4.06 |

Baker inspection of one-cube scenes accepts the atmosphere and constant
sources, records model version 2 with the environment's SH window, and rejects
stale or conflicting sky fields.

Sun authoring adds CPU checks against Kim et al.'s Planckian-locus spline, within
0.004 in CIE xy from 2000 K to 20000 K, plus range, loader-conflict and
ignored-directional-light tests. The `atmosphere_bistro_temperature_local` case
at 5778 K keeps the default case's sunlit-facade luminance, 0.1225 against
0.122, with a warmer red-to-blue ratio of 1.74 against 1.42. Repeated Bistro
snapshots previously flipped between two lighting states per capture because
the Metal IBL bake could read a partly written atmosphere source. With the
queue barrier, two runs differ in at most 33 pixels per view.

The per-frame sun adds CPU checks against Beer-Lambert optical depth: for a
Rayleigh-only zenith sun the observer irradiance matches `solar *
exp(-beta H (1 - exp(-thickness / H)))` within 0.2%, a clear medium passes the
calibrated irradiance exactly and a set sun gives zero. Others check that
glow reaches only the sky record, that a moving light redraws at once but
requests a revision at most once per interval after publication, that an
unstarted request follows it immediately and that the final sun gets its own
revision. Metal API validation of `clouds_bistro_sky_local` passes without
diagnostics (report
sha256:05d72c17d51b51bd9a016094e2a34c0de1b65976f7140c11ef75d9b0d5a060a2). The
light-driven and authored 5778 K suns still agree to an HDR ratio of 1.000000
and a final-colour mean absolute error of 2.1e-6. In Bistro the per-frame CPU
irradiance keeps every view inside the baseline gate; only the glow changes
view 13, around a sun just behind the clouds (mean absolute error 1.21e-3 in
two runs). That look was re-accepted as generation
`sha256:5a10ac6d9881514da1cea7b107f35afa9461533ca1c46701b07a718542cfabbf`,
which a fresh run matches with failed-pixel ratio 0 and mean absolute error at
most 8.4e-7.

The directional sun light adds CPU checks that the sun points against a rotated
light, takes its tinted irradiance, ignores unflagged lights, keeps the
authored disc for a hard-edged light, requests nothing for an unchanged light
and restores the authored sun without one, plus loader checks of the
temperature domain and flag. `sun_light_bistro_local` drives the sun from a
light rotated a quarter turn about +Y at 5778 K and the default luminance;
against `atmosphere_bistro_temperature_local`, which authors the same sun in
the atmosphere block, mean HDR radiance agrees to a ratio of 0.999999, the mean
relative difference is 8.4e-6 and final colour differs by a mean absolute error
of 2.1e-6. `sun_light_bistro_low_sky_local` looks toward a 3200 K light 8
degrees above the horizon and shows the disc and a warm horizon glow around
it. Bistro without its sun entity still matches generation `74b6e5c5` (mean
absolute error at most 6.3e-7, no failing pixels), and with the entity differs
from that generation by at most 1.13e-6 per view. The entity changes the
workload fingerprint, so the unchanged look was re-accepted as generation
`sha256:d3a548c56cfb58bc3689ef7e421b7c4e5530204d38f6a0dc3b4febe0e6a69046`.

The camera-dependent sky adds CPU checks of the camera altitude and world-scale
boundary and a graph-capacity check at 165 passes. The
`atmosphere_bistro_sky_local` case looks at the sun-side sky from 30 m. Against
the removed cube sky rendered at the same 30 m, open sky more than one degree
from the horizon and two degrees from the sun differs by at most 0.65%
(mean 0.07%) over 648,208 pixels, and the ground below the horizon by at most
0.33%. Within one degree of the horizon the lookup resolves the horizon more
sharply than the cube's texels. At 100 metres per world unit, a Bistro facade
about 8 km away loses red and gains blue as expected: RGB 0.0749, 0.0859,
0.0769 becomes 0.0646, 0.0814, 0.0906. The clearcoat furnace passes unchanged
at maximum error 0.00049. Metal API validation of the atmosphere Bistro case
reports no diagnostics. On the M1 Pro at 1280×720, a non-authoritative
five-process steady Bistro profile measures the sky-view lookup at 0.0695 ms,
the aerial volume at 0.0395 ms and the aerial-only fog pass at 0.0859 ms, each
from 1,500 valid samples. A matched pre-change timing report is unavailable.
Vulkan SPIR-V validation and layout reflection pass in
[the retained diagnostic](../../assets/verification/renderer-features/atmosphere-spirv.txt).
The Bistro Metal text baseline was re-accepted at the settled look of the sky
system, including the cloud layer of ADR-074: generation
`sha256:74b6e5c517c668ed17354a0d3f1026bc0d0767b82b17666727fb9c1ca3f4c19c`,
succeeded by the sun-entity generation above.
The Bistro Vulkan text baseline and native Vulkan runs remain unavailable.
