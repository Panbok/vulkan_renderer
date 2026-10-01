---
status: implemented
updated: 2026-10-01
authority: adr
---

# ADR-081: Physical night sky

## Status

Accepted and implemented: pre-exposure, the moon as a second atmosphere light
and the procedural star field. Native Vulkan runs on Windows; a same-revision
Metal/Vulkan comparison remains pending.

## Context

A day/night cycle needs night lighting that stays physically consistent with
the day. The atmosphere ([ADR-058](058-revision-baked-sky-atmosphere.md)) has
one light, so once the sun sets the sky and scene go black.

At a physical scale, moonlight is about 2^-19 of sunlight. Automatic exposure
must reach that range, and RGBA16F histories must still resolve it: their
smallest normal value is 2^-14, and smaller values lose precision until they
flush to zero below 2^-24. The user chose a physical brightness scale with
pre-exposure over an artistic night tint, the moon as a second atmosphere
light over a sky-only disc, and a procedural star field over a cube map.

## Decision

### Pre-exposure

The renderer stores HDR radiance multiplied by a pre-exposure P = 2^k, with k
an integer.

- **Exactness.** P is a power of two, so scaling and its inverse are exact.
- **Choosing k.** k comes from the newest completed exposure multiplier E in
  automatic mode, or from the manual multiplier. While the exposure is within
  four stops of 1, P is 1 (`VKR_PRE_EXPOSURE_NEUTRAL_STOPS`).
- **Hysteresis.** Outside that band, k becomes round(log2 E) and is kept while
  log2 E stays within one stop of it. Returning to P = 1 needs a further half
  stop.
- **Bounds.** k is clamped to ±24 (`VKR_PRE_EXPOSURE_MAX_STOPS`).
- **Effect on existing content.** Ordinary scenes keep P = 1, so existing
  baselines are unchanged.

The prepared packet stays physical, because temporal, SSR, SSGI and froxel
history signatures hash unscaled inputs. P is applied where values are lowered
into GPU constants:

- frame constants: the directional light, ambient light and IBL gain;
- punctual and rectangle light rows, and probe intensities;
- the sky record's solar radiance and the constant sky radiance;
- the fog colour and world-text colour.

Shaders multiply by P only where a value reaches HDR without passing through
those constants: material emissive, the baked diffuse volume and the no-sky
fallback colour.

Consumers that need physical values divide P back out:

- **Tonemap.** Divides by P before applying the exposure.
- **Metering.** Shifts its log-luminance window, middle grey and background
  threshold by k, so the resolved exposure stays absolute.
  - The default window is [-24, 8] log2, 32 stops over 256 bins, or 8 bins per
    stop. A whole-stop P therefore shifts the histogram by whole bins.
  - The EV range is [-8, 24] and the background threshold is 2^-28.
- **Bloom.** Scales its threshold, knee, knee epsilon and firefly clamp by P.
  The Karis weight uses L/P.
- **TAA.** Scales its transmission-reactivity luminance floor by P.
- **Upscalers.** MetalFX and FSR 3.1 receive P as their `preExposure`.

Each backend records the exponent of every submission in a 32-entry ring. TAA,
SSR, SSGI, cloud, froxel-injection and MetalFX/FSR stabilization histories are
rescaled on reuse by 2^(k_now − k_producer), which is also exact. A producer
outside the ring is treated as this frame's scale.

Each texture records the scale its IBL bake was written at
(`radiance_stops`). The global IBL gain is P/B_global, and each probe's
intensity is multiplied by B_global/B_probe, because the frame gain already
applies to probes.

Inspection render modes write non-radiance values into the HDR target, so they
force P = 1. `VKR_PRE_EXPOSURE_FORCE_STOPS=<k>` forces an exponent for
diagnosis. Captures record P in their sidecar as `pre_exposure`, and the
harness preview of a scene-referred HDR channel divides by it.

### The moon

A directional light whose `atmosphere_moon` flag is set is the moon, the
atmosphere's second light, as UE5's atmosphere light index 1 is.

- **Flag and precedence.** The flag defaults to false. A moon light is never
  the sun, even with its default `atmosphere_sun`, so flagging a light as the
  moon takes one edit. Without an enabled atmosphere there is no moon.
- **Choosing the moon.** Among enabled, visible moon lights the sun's rule
  applies: the lowest render id, else the first found. A scene without its own
  moon uses the World's while it inherits the World. The loader warns about
  more than one.
- **Light values.** The light's colour, tinted by its temperature, times its
  intensity is the full moon's top-of-atmosphere irradiance. Its diameter
  sizes the disc and the shadow penumbra; Details labels it Moon diameter and
  disables the sun flag.
- **Phase.** The moon's irradiance is scaled by its lit fraction,
  (1 − cos θ) / 2, where θ is its angle from the sun. A physical full moon is
  about 2.5 × 10^-6 of the sun, which the night cases author as 4.5 × 10^-6
  beside the default sun's luminance of 1.77.

Both lights shape the sky:

- **Scattering.** The source-cube bake and the aerial-perspective volume
  integrate both lights in one march. Each light adds its own single
  scattering, multiple-scattering lookup and lit ground. The lookups are per
  unit irradiance, so the moon reuses them.
- **Sky view.** The sky-view image holds a sun-relative and a moon-relative
  table side by side. Each is lit by its own light alone and parameterized
  around that light's azimuth, so the moon's halo keeps the resolution the sun
  has. A light without irradiance leaves its table black, and the background
  skips sampling it.
- **Disc.** The background adds the moon disc and its glow. The disc shows the
  sunlit half of a sphere: a view ray reaches the surface point whose normal
  faces the observer, which is lit when that normal faces the sun. Lit points
  share the full-moon radiance, so the disc integrates to the phase-scaled
  irradiance, and a 0.05 cosine ramp antialiases the terminator. A moon less
  than 10^-4 lit is not drawn.

The renderer still lights with one directional light. It is the key light:

- **The sun** while it lights the observer.
- **Otherwise the moon,** while the moon is above the horizon.

The key light drives direct lighting, cascaded shadows, fog in-scatter and
the cloud shadow map. The cloud trace adds the moon only while it is the key
light, and still lights high cloud with a sun just below the horizon. A sample
the planet hides from a light skips that light's march.

A key light too dim to show is disabled, along with its cascades.

- **Test.** The runtime compares E × exposure / π, the display value of a
  white Lambertian surface it lights head-on, against 2^-12. That is a
  sixteenth of an 8-bit step (`VKR_LIGHTING_KEY_LIGHT_VISIBLE_MIN`).
- **Exposure.** The exposure is the one the previous frame displayed with
  (`vkr_renderer_get_display_exposure`). Before any frame there is no
  exposure, and the light stays enabled.
- **Why.** Under Bistro's street lamps the full moon's direct light is about
  2^-16 of display white. Its four moving cascades nonetheless cost 8.4 ms of
  GPU time per frame.

Sky-light bakes are skipped only while both lights are past the dark
depression. The offline diffuse baker applies the same moon, both-light source
and key light. Its recipe hash covers the moon only when the moon has
irradiance, so the hashes of moonless bakes are unchanged.

### Stars

The deferred background draws a procedural star field behind the atmosphere.

- **Layout.** Each face of a cube around the celestial pole holds 64 × 64
  cells, and a cell holds at most one star (37 % do). A star's hashed
  position keeps 15 % of a cell from its border, so its footprint lies in its
  cell and the three neighbours toward the nearest corner. A pixel evaluates
  those four cells.
- **Magnitudes.** Magnitudes follow the naked-eye count law
  N(< m) ∝ 10^(0.5 m) up to magnitude 6.5: about 9000 stars, the brightest
  near −1.5.
- **Brightness.** A magnitude-m star's irradiance is
  `star_intensity` × 10^(−0.4 (m + 26.74)) times the default sun's luminance.
  At 1 the field is physical beside that sun.
- **Colour.** Each star takes one of four unit-luminance classes: blue-white,
  white, yellow or orange.
- **Shape.** Each star is a Gaussian 0.7 pixel wide, never narrower than
  10^−4 radians, so temporal jitter resolves it without flicker.
- **Sky effects.** The view transmittance attenuates and reddens stars near
  the horizon, and clouds composite over them. Stars do not light the scene
  and do not enter the sky-light bake.

The field keeps its place relative to the sun:

- **Rotation.** Its frame is the celestial pole and the sun's direction
  projected off the pole, so as the sun moves through its hour angle about the
  pole, the stars rise and set with the night.
- **Visibility.** The field draws only while the sun is below the observer's
  horizon, where stars can show; in daylight it is skipped.
- **Authoring.** The Sky atmosphere component's Night group authors
  `star_intensity` in [0, 10^6] (default 1) and `celestial_pole`, a direction
  whose elevation is the latitude (default 45 degrees, toward +Z, so the noon
  sun lies toward −Z). Both follow the scene at once and never request a
  bake.
- **Sky record.** It carries `star_pole` and `star_axis` and grows to 432
  bytes. The magnitude-zero irradiance is pre-exposed.

## Consequences

- **Precision.** Night scenes keep RGBA16F precision.
  - In the Bistro night case at P = 1, 834 pixels are zero and 29,381 are
    subnormal. At P = 8 there are 550 and 3,801.
  - These recovered dark pixels change the metered percentile slightly. At
    converged exposure, P = 8 meters 0.16 % brighter than P = 1. This is the
    precision gain, not a scaling error.
- **Day output.** With P = 1, day output is unchanged.
- **Metering defaults.** The widened window and EV range change automatic
  exposure where scenes previously hit the old +4 EV clamp or contained
  luminance below 10^-4.
- **P below 1.** P < 1 is reached only above roughly 22× over-exposure. At a
  forced P = 1/64, about 6 to 17 isolated pixels per Bistro view move across
  fp16 subnormal or absolute-epsilon boundaries.

### Evidence

Metal Release, M1 Pro:

- **Bistro text snapshot** (`tools/cases/smoke/bistro_metal_text_snapshot.case.json`):

  | Forced P | Report digest | Result |
  |---|---|---|
  | 1 | `sha256:2a2c4275…`; final build `sha256:d04c688b…` | Pass |
  | 8 | `sha256:80558184…` | Pass, per-view errors matching P = 1 |
  | 1/64 | `sha256:a5905cca…` | Pass |

- **Bistro night case** (`tools/cases/local/atmosphere_bistro_night_local.case.json`):
  - Automatic exposure converges near 10.1.
  - Physical HDR, post-TAA HDR and bloom match between P = 1 and P = 8 to a
    1.000000 luminance ratio. Bloom is 1.000004 before the knee-epsilon fix.
  - Final colour differs in 5 of 921,600 pixels (`sha256:6fe80083…` and
    `sha256:752cf89a…`).
  - Before the TAA floor was scaled, 1,917 pixels differed.
- **Validation.**
  - One Metal API validation process on Bistro (automatic exposure, bloom,
    TAA, SSR and GTAO at P = 8) reports no errors.
  - The CPU suite passes.
  - All 102 production SPIR-V modules pass `spirv-val`. Compiled offsets
    match the C roots for TAA (144), SSR temporal (496), SSGI temporal (360),
    froxel inject (44), cloud trace (120) and FSR stabilize (40).

Moon evidence (Metal Release, M1 Pro):

- **Day output.** The Bistro atmosphere day case
  (`tools/cases/local/atmosphere_bistro_local.case.json`) was captured before
  the moon (`sha256:00618d07…`) and after it (`sha256:8ddd6b2c…`).
  - The mean HDR luminance ratio is 0.999999, with a largest pixel difference
    of 0.0236.
  - A repeat run at the same build (`sha256:a7a8e5ac…`) differs by up to
    0.026, so the change is within run-to-run noise.
- **Night street.** `tools/cases/local/atmosphere_bistro_night_local.case.json`
  (`sha256:571a138f…`): the street lamps still set the exposure, now 10.8.
  - Moonlit sky pixels that were zero now meter, which raises the exposure.
  - The moon disc is drawn, at a peak HDR radiance of 0.055.
  - HDR elsewhere matches the moonless run to a 1.000066 mean ratio.
- **Moonlit sky.**
  `tools/cases/local/atmosphere_bistro_moonlit_sky_local.case.json`
  (`sha256:5fd71c85…`) uses manual exposure 2^18, so P = 2^18.
  - It shows a dark blue sky with the moon's Mie halo and glow.
  - Lamp-lit surfaces saturate below the RGBA16F maximum. There are no
    non-finite values before or after TAA and bloom.
- **Moonlit clouds.** `tools/cases/local/clouds_bistro_night_local.case.json`
  (`sha256:e2f1e388…`) shows moonlit clouds and the moon-projected cloud
  shadows.
- **Validation.**
  - One Metal API validation process on that case reports no errors
    (`sha256:fb668b06…`).
  - CPU tests check that a moon keeps a dark-sun bake from being skipped,
    against the independent CPU baker, and that the moon is never the sun and
    is the key light only while the sun is below the horizon.
  - All 102 SPIR-V modules pass `spirv-val`, and the compiled atmosphere root
    (208 bytes) and sky record (448 bytes) offsets match the C asserts.

Star evidence (Metal Release, M1 Pro):

- **Starry sky.**
  `tools/cases/local/atmosphere_bistro_starry_sky_local.case.json`
  (`sha256:e5329184…`), a moonless sky at manual exposure 2^19, shows the field
  thinning and reddening toward the horizon.
- **Moonlit clouds.** The moonlit-cloud case (`sha256:8df4adeb…`) shows the
  brighter stars between the clouds.
- **Day output.** The day atmosphere case matches the pre-moon reference to a
  1.000000 mean HDR luminance ratio, with run-to-run noise of up to 0.026.
- **Validation.** One Metal API validation process on the starry case is clean
  (`sha256:3dd357d4…`). A CPU test checks that the field's frame turns with
  the sun about the pole, and that the field is hidden in daylight and without
  intensity. The compiled sky offsets match the C asserts: `star_pole` at 400,
  `star_axis` at 416, and a 480-byte Vulkan record.

Day/night cycle evidence (Metal Release, M1 Pro). These are diagnostic,
single-process runs with no timing authority. A temporary diagnostic, since
reverted, turned the sun and moon together about the celestial pole by 0.25
degrees per frame for 1440 measured frames.

- **Street exposure.** In the Bistro street (night case), the automatic
  exposure ran from 3.0 at noon to 10.9 at night, and P stayed 1 throughout.
  The street lamps keep the night within four stops of the day.
- **Sky exposure.** In the cloud-sky view (moonlit-cloud scene, automatic
  exposure):
  - The exposure ran from 1.5 at noon to 2.7 × 10^4 at the end of the night,
    where P = 2^16.
  - P changed 27 times, always by one stop or across the neutral band (5 to 0
    and back).
  - Through the 2^5 to 1 change at sunrise, post-TAA physical luminance stays
    within 3 % frame to frame (0.0274, 0.0282, 0.0278, 0.0274). A wrong
    history scale would jump by 32×.
- **GPU cost.** GPU submission time by sun elevation (night below −12
  degrees), p50 / p95:

  | Run | Sun up | Night |
  |---|---|---|
  | Moon, before the invisible-light rule | 33.1 / 36.9 ms | 33.2 / 36.9 ms |
  | Moon, with the rule | 33.1 / 36.9 ms | 24.8 / 28.7 ms |
  | Moonless | 33.1 / 36.5 ms | 24.8 / 28.2 ms |

  The remaining 0.5 ms at p95 is the moonlit sky-light bake, which now runs at
  night at up to 4 Hz.
- **Output after the rule.** The night street matches its pre-rule output to a
  0.999999 mean HDR ratio, with 4 final pixels differing as run-to-run noise
  (`sha256:ef33532c…`). The moonlit-cloud (`sha256:5a248d2e…`), moonlit-sky
  (`sha256:26e9cf1b…`) and starry-sky (`sha256:606f6e70…`) cases keep their
  key light, and the Bistro text baseline passes (`sha256:feac04ef…`).

On Windows (RX 6700 XT), the night, moonlit-sky, starry-sky and moonlit-cloud
cases render natively on Vulkan with the expected pre-exposure and clean Debug
validation. Without a same-revision Metal comparison, the affected shader
domains stay UNALIGNED in [ADR-044](044-shader-cross-backend-contract.md).
The four cases leave their backend unpinned, so each machine runs its native
backend.

## Alternatives considered

- **Scaling the packet.** Scaling the prepared packet instead of its GPU
  lowering would change every history signature on each exposure step.
- **Continuous pre-exposure.** Tracking E exactly makes history rescaling
  inexact, and the scale changes every frame.
- **Artistic night tint.** It keeps the day exposure but gives up the physical
  relationship between sun, moon and artificial lights.

## Revisit when

A backend gains a float32 HDR path, or matched evidence shows that an
exposure-relative quantity other than those listed here is not scale-free.

## Implementation

[`vkr_exposure.c`](../../renderer/src/vkr_exposure.c),
[`vkr_atmosphere.c`](../../renderer/src/vkr_atmosphere.c),
[`atmosphere_kernel.slangh`](../../renderer/src/shaders/shared/atmosphere_kernel.slangh),
[`vkr_scene_system.c`](../../runtime/src/renderer/systems/vkr_scene_system.c),
[`vkr_lighting_system.c`](../../runtime/src/renderer/systems/vkr_lighting_system.c),
[`vkr_bake_atmosphere.cpp`](../../tools/bake/vkr_bake_atmosphere.cpp),
[`vkr_packet_constants.c`](../../renderer/src/vkr_packet_constants.c),
[`vkr_bloom.c`](../../renderer/src/vkr_bloom.c),
[`vkr_renderer.c`](../../renderer/src/vkr_renderer.c),
[`vkr_metal_packet_frame.inc`](../../renderer/src/metal/internal/vkr_metal_packet_frame.inc) and
[`vkr_vulkan_deferred.c`](../../renderer/src/vulkan/vkr_vulkan_deferred.c).
