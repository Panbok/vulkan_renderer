---
status: implemented
updated: 2026-09-08
authority: adr
---

# ADR-053: Energy-compensated GGX with a shared DFG

## Status

Accepted.

## Decision

Use height-correlated Smith GGX visibility for direct lighting and a matching
Schlick A/B integral for environment lighting. Both native renderers consume
one immutable 256×256 RG16F table. Its axes are `sqrt(N·V)` and perceptual
roughness, sampled at endpoint texel centers with linear clamp filtering.
The nonlinear view coordinate resolves the narrow grazing region without
increasing storage. GGX width remains `alpha = roughness²`.

Let `E = A + B`. Scale direct single-scatter specular by
`1 + F0 * (1/E - 1)`, and use `(F0*A + F90*B)` times that scale for the
prefiltered environment. Preserve the existing low-reflectance Schlick endpoint
`F90 = saturate(25 * max(F0))`. Prepare this record once per surface, before
light and probe loops, including deferred, forward and transmission paths.

Reserve `1 - integrated_specular_reflectance` for diffuse, then apply base
color and the nonmetal fraction. Transmission composition uses the same
integrated reflectance for remaining transport. Keeping the old per-light
Schlick diffuse weight alongside the compensated specular lobe exceeded unit
furnace energy at grazing views in the numerical baseline.

This is a scaled-GGX approximation with a directional energy allocation. It
is not an exact reciprocal multiple-scattering BSDF or a converged light
transport solution. The prefiltered cubemap still uses split-sum approximations
for nonuniform environments. Existing roughness floors, normal filtering,
SH `E/π` normalization and light units remain in force.

[ADR-062](062-layered-clearcoat.md) extends this allocation to an independent
fixed-F0 GGX coat. Its integrated reflectance attenuates base transport before
adding the coat lobe; each layer retains its own normal and roughness.
[ADR-063](063-charlie-sheen.md) adds a Charlie sheen layer between the base and
coat, using separate directional energy and allocation reserves.

## Ownership and generation

The renderer owns the 256 KiB native image until completion-safe destruction.
Metal uploads it during setup. Vulkan stages it on the first submitted frame,
then releases staging only when that submission completes. The table is sampled
through the native frame root; no material field or per-draw allocation is added.

[`vkr_dfg_cooker.cpp`](../../tools/vkr_dfg_cooker.cpp) integrates 4096 visible
GGX normal samples per texel in double precision and emits the shared half-float
coefficients. The build wrapper owns generation. The table includes the analytic
zero-roughness endpoint and a positive grazing limit. Native root layouts and
reflection requirements belong to [ADR-044](044-shader-cross-backend-contract.md).

The visibility and compensation conventions follow
[Filament's material model](https://google.github.io/filament/Filament.md.html);
visible-normal sampling follows
[Heitz 2018](https://jcgt.org/published/0007/04/01/paper.pdf).

## Evidence

Independent numerical integration gives `1 - ln(2) = 0.3068528194` for the
single-scatter white conductor at roughness 1 and normal view. The baked value
is 0.3069183826. Across the selected roughness/view grid, the largest coefficient
error against 262144 samples is 0.000302827. Independent solid-angle quadrature
agrees within 0.000081038. Combined unit-albedo furnace energy reaches at most
1.000330261, including lookup and integration error.

A native Metal Release furnace with unit environment radiance covers six
roughnesses, dielectric and metal materials, and opaque, blended and transmitting
paths. All 36 center samples remain within 0.000489 of unit scene-linear RGB.
Metal API validation passes. Shader validation stopped in MetalTools while decoding
a GPU report, leaving the underlying diagnostic unresolved. Windows/Vulkan native
execution remains unavailable; compiled SPIR-V does not establish bilateral parity.
