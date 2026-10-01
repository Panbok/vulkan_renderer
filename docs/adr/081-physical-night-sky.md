---
status: partial
updated: 2026-10-01
authority: adr
---

# ADR-081: Physical night sky

## Status

Accepted. Pre-exposure is implemented. The moon as a second atmosphere light
and the procedural star field are pending.

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

Native Vulkan execution and the Windows-only FSR SDK build are unavailable on
this host, so the affected shader domains are UNALIGNED in
[ADR-044](044-shader-cross-backend-contract.md).

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
[`vkr_packet_constants.c`](../../renderer/src/vkr_packet_constants.c),
[`vkr_bloom.c`](../../renderer/src/vkr_bloom.c),
[`vkr_renderer.c`](../../renderer/src/vkr_renderer.c),
[`vkr_metal_packet_frame.inc`](../../renderer/src/metal/internal/vkr_metal_packet_frame.inc) and
[`vkr_vulkan_deferred.c`](../../renderer/src/vulkan/vkr_vulkan_deferred.c).
