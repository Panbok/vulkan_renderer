---
status: implemented
updated: 2026-09-08
authority: adr
---

# ADR-058: Revision-baked sky atmosphere and unified sun

## Status

Accepted. Runtime publication and offline diffuse transport are implemented.
Native Vulkan execution remains unavailable on the development host.

## Context

Outdoor sky, global indirect lighting and directional sunlight must describe the
same atmosphere. A continuously updated camera-dependent atmosphere would change
the resource and frame budgets beyond the accepted first stage.

## Decision

Bake the global environment at an authored observer altitude when atmosphere or
sun settings change. Camera motion does not request another bake. The atmosphere
replaces the global HDR source while enabled; local reflection probes retain
their separately prepared sources.

Each renderer owns two reusable RGBA16F lookup textures: a 256×64 transmittance
texture and a 32×32 multiple-scattering texture, totaling 136 KiB. A candidate
owns a 256-pixel-face source cubemap and the existing GGX prefilter/SH delivery.
The bake uses 40 samples per transmittance texel, 64 directions with 20 samples
per multiple-scattering texel, and 32 samples per source texel. A 4×4 subpixel
coverage estimate resolves the visible sun disc at the accepted coarse cubemap
resolution.

Source RGB contains atmospheric in-scatter. Source alpha contains sun-disc
coverage. The sky adds that coverage times the published solar radiance; GGX
prefiltering and diffuse SH consume only RGB. Direct sunlight therefore does
not also enter the global indirect lighting through the disc.

One sun direction, angular diameter and attenuated RGB irradiance drive the sky,
directional lighting, cascaded shadows and contact hardening. A GPU query of the
same transmittance lookup supplies observer irradiance. Disc radiance is that
irradiance divided by the projected solid angle `π sin²(angular radius)`.
Authored top-of-atmosphere irradiance is uniformly scaled when necessary to keep
peak disc radiance at most 60000, preserving RGB ratios and RGBA16F headroom.
Source radiance is uniformly limited to 5000 so its sum with the visible disc
also fits RGBA16F. These are scene-linear calibration values, not lux. The supported Earth model
accepts density multipliers in `[0,100]`, solar diameters in `[1e-16,5]` degrees,
observer altitudes in `[0,100000]` metres and Mie anisotropy in `[-0.95,0.95]`.
The input boundary rejects values outside this numerical domain.

A revision prepares distinct candidate resources while the prior generation
remains active. Submission does not establish readiness. The scene publishes
source, prefilter, SH and sun together only after native completion confirms the
whole candidate. Failure or cancellation retains the previous generation.
The native source owns a transient 16-byte sun readback until completion and
consumption; ordinary readiness queries do not wait.

The offline diffuse baker prepares the same lookup model and an immutable
scene-owned RGB cubemap before tracing. It derives one attenuated sun, suppresses
the replaced HDR source and authored directional lights, and retains local lights.
Glass, multibounce paths and photon caustics consume that lighting through the
existing transport integrator. Bake metadata records the model version and
parameter hash. Existing version-one sidecars without atmosphere provenance
remain usable for scenes that do not enable atmosphere.

## Consequences

Stable settings add no atmosphere integration work to ordinary frames. Revisions
pay the sky and existing IBL bake cost and temporarily retain both generations.
Lighting is evaluated at the authored altitude even when the camera moves.
The source resolution limits sun-disc detail. This stage does not provide aerial
perspective, local participating media or volumetric light shafts; analytic fog
remains governed by [ADR-057](057-analytic-height-fog.md).

## Alternatives considered

A separate authored directional sun can disagree with the sky and shadows.
Including the disc in both IBL RGB and direct lighting counts its energy twice.
Per-frame sky integration and higher-resolution cubemaps exceed this stage's
accepted update and storage scope.

## Revisit when

Camera-dependent aerial perspective, changing weather, higher-resolution solar
reflections or volumetric fog require a different update or storage budget.

## Implementation

[Public settings](../../renderer/src/vkr_atmosphere.h),
[shared atmosphere math](../../renderer/src/shaders/shared/atmosphere_kernel.slangh),
and [publisher contract](../../renderer/src/vkr_asset_publisher.h).
The [CPU atmosphere baker](../../tools/bake/vkr_bake_atmosphere.cpp) owns its
temporary LUTs and scene-lifetime source cube.

Metal Release captures cover zero extinction, day, sunset, elevated observer,
disabled fallback and Bistro. A focused native API-validation fixture covers
completion, superseded candidates, failure, disable and reload. The zero-medium
disc integrates to 0.99483 for authored unit irradiance at the accepted cubemap
resolution. A CPU/GPU day-sky comparison outside two degrees of the sun has
maximum RGB absolute error 0.000853. A glass-ceiling smoke bake produces 27 valid
probes, eight interpolation cells and 9,072 caustic deposits from 20,000 photons.
These checks establish the available native path and bounded transport fixture;
SPIR-V compilation and host checks do not establish native Vulkan parity.
