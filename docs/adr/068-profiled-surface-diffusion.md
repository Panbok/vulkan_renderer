---
status: implemented
updated: 2026-09-08
authority: adr
---

# ADR-068: Profiled surface diffusion

## Status

Accepted. Runtime and offline transport are implemented. Native Metal checks
pass; native Vulkan execution remains unavailable.

## Context

Thin-sheet diffuse transmission supplies backlighting but cannot spread lighting
across a skin or wax surface. The accepted addition uses authored diffusion
distances and a bounded screen-space gather, with the same surface profile in
the offline baker. It remains optional and disabled by default.

## Decision

The user approved optional surface diffusion for skin and wax: eight RGB
profiles, material strength, 32 samples, a 32 internal-pixel radius cap, and
matching offline BSSRDF transport. This models diffusion across opaque surfaces.
It does not model transmission through thin ears or finite wax solids. Blended
and refractive materials remain supported elsewhere in baked scenes.

Scenes author `subsurface: { enabled: true, profiles: [[r, g, b], ...] }`.
Each RGB vector gives positive finite effective diffusion distances in metres.
Missing or false `enabled` disables the feature. Active scenes require one to
eight profiles. Materials author `subsurface_strength` in [0,1] and integer
`subsurface_profile` in [0,7]. Active materials require opaque/cutout PBR without
refraction, authored thickness or thin-sheet diffuse transmission. A material
whose profile is absent from the scene keeps local diffuse shading.

[Frame input](../../renderer/src/vkr_frame_input.h) version 44 borrows the
scene-owned profile binding. Native preparation validates the immutable texture
and registers its normal GPU last use. A zero profile count and diagnostic
render modes disable the passes. Material rows append one float4, reaching
352 bytes on Metal and 288 on Vulkan.

The [shared profile helper](../../renderer/src/vkr_subsurface.c) evaluates the
normalized diffusion profile

`R(r,d) = (exp(-r/d) + exp(-r/(3d))) / (8*pi*d*r)`.

Its radial mass is a mixture of exponential distributions: 25% with scale d,
75% with scale 3d. The caller-owned 65×8 RGBA32F bank stores 32 antipodal RGB
importance taps per profile and their distance scales. Offsets use units of
the largest RGB distance. Runtime uniformly narrows the projected profile to
fit the 32-pixel radius cap; offline sampling retains the full tail.

## Lighting and transport

Let C be the nonmetal diffuse albedo, s the authored strength, and B the existing
outgoing GGX diffuse reserve multiplied by coat and sheen transmission.
Deferred lighting writes `F = sqrt(C) * E`, where E is uncolored diffuse
irradiance in the renderer's diffuse-response convention. Light helpers retain
the irradiance they already evaluated; they do not repeat shadow queries or
rectangle integrals. SSGI adds its corresponding irradiance to F with its
existing confidence, AO and baked-volume coverage policy.

The gather couples two points with `min(s_receiver, s_source)`. It filters F
within one object and profile, rejecting depth/normal discontinuities. All four
bilinear metadata texels must be valid; their minimum strength gives a
conservative footprint coupling. Kernel normalization does not normalize away
strength. The final update is equivalent to

`HDR + B*sqrt(C) * (filtered(min_strength * F) - s*F_center)`.

The implementation accumulates weighted differences, so a constant field with
constant strength cancels before HDR addition. Specular and emission stay in
HDR. Global/probe/volume diffuse keeps its existing AO policy. The pass runs
after SSGI, before SSR, fog, transparency and temporal reconstruction. SSR and
refraction therefore see the composed diffuse appearance.

The offline model uses the same radial profile and material coupling, with
the existing angular reserve at the outgoing endpoint:

`S(xo,xi,wo) = R(distance) * min(so,si) * sqrt(Co*Ci) * B(wo) / pi`.

It inherits the existing outgoing-only angular approximation; this is not a
new reciprocal model for the complete layered BSDF. Runtime adds projected
screen-space support, geometric rejection and finite sampling approximations.
Its source irradiance also uses the current view's angular lighting approximation.
Source stores saturate signed F to the finite half-float range, ±65,504. Extreme
irradiance can therefore lose diffusion energy even when the final albedo-scaled
lighting would fit; the approved image format and sample budget are unchanged.

The [offline integrator](../../tools/bake/vkr_bake_integrator.cpp) samples the
full radial tail with a uniform RGB channel and three projected disk axes
(probabilities 1/2, 1/4, 1/4). Whole-bounds chords use a bounded reservoir over
eligible intersections on the same object, profile and oriented surface side,
including alpha-cutout visibility. The combined area PDF includes each axis's
intersection count and absolute geometric Jacobian. Profile evaluation uses the
actual three-dimensional endpoint distance. Zero-strength geometry remains in
the sampling measure; its strength coupling contributes zero.

Active paths choose local or spatial transport with equal probability and weight
the selected direct, caustic and continuation contributions accordingly. Spatial
entries evaluate incident irradiance without applying the full BSDF a second
time. Direct lighting includes one incident cosine; photon flux density already
contains that cosine. The Lambert factor appears once. External cosine
continuation preserves the existing depth, roulette and medium-stack rules.
Neither relocation nor that continuation crosses a medium boundary. Inactive
materials preserve the preceding random-number sequence.

## Consequences

### Ownership and budget

Two graph-owned RGBA16F images at internal Scene resolution hold F and composed
HDR. Existing per-image completion and resize retirement own their lifetime.
They require 42.1875 MiB for three 1280×720 sets or 112.5 MiB for eight. The
immutable bank adds 8,320 bytes, bringing the totals to 42.195435 and
112.507935 MiB, plus 16 bytes per material row. No temporal history is added.

Scene finalization builds the bank in bounded stack storage. Texture publication
copies the borrowed bytes before returning. The texture system owns the bank;
the scene owns one reference. Async retries retain only eight profile inputs
and regenerate the bank. The content key includes every authored float bit.
Upload estimates add 8,320 bytes and one operation. Release consumes the scene
reference; failed native destruction remains registered under the texture
system's existing final-destruction ownership.

The gather parameter block is 32 bytes; native roots are 208 bytes on Metal and
192 on Vulkan. Deferred roots are 240/192 bytes and SSGI roots 496/432 bytes
(Metal/Vulkan), including the optional source image and profile count. Disabled
scenes allocate neither diffusion graph image.

### Approximation limits

The normalized diffusion profile is a planar surface model. Several close folded
or stacked sheets belonging to one eligible object/profile add kernel mass;
their total area integral can exceed one. The same-side test rejects opposite
walls but does not establish finite-volume energy conservation. Runtime adds
screen-edge normalization, depth/normal rejection, a 32-pixel cap and finite
sampling. These limits preclude claims of physical transmission through ears
or solid wax. The extreme-irradiance source truncation described above is a
separate half-float limit.

## Evidence and limits

Independent profile integration gives mass error 3.48e-9 and mean-radius error
5.21e-9. The table preserves the constant white field and antipodal centroid.
A shared endpoint oracle covers 153,015 heterogeneous-material cases without
energy gain; maximum numerical error is 1.11e-7. A separate comparison against
the preceding offline BSDF preserves 4,800 zero-strength samples bit-for-bit
and restores homogeneous diffuse allocation in 14,400 cases, with normalized
error 1.75e-7.

The independent projected-disk/BVH oracle samples 400,000 planar and 500,000
multi-intersection configurations. Planar mass is 0.499691/0.500706/0.501463
against 0.5; tilted mass differs from its independent integral by at most 0.248%.
A 150,000-path white-environment trace is within two standard errors of one.
Eligibility, zero strength, distances down to 1e-40 metres, and photon density
without a second incident cosine pass. The tilted case also demonstrates the
folded-sheet limitation above.

The normal Release wrapper compiles both production shader paths. Reflection
and `spirv-val` pass for the actual gather, deferred and SSGI composite modules,
including the 32-byte parameters, 192-byte Vulkan gather root and material row.
These are compilation and ABI results, not native Vulkan execution.

Five 513×321 Metal fixtures pass: enabled, disabled, zero strength, white furnace,
and mixed SSR/SSGI/glass/coat. The checker measures the shadow transition widening
from 7.7635 pixels to RGB widths 13.2813/9.3432/8.4262. Disabled and zero-strength
final color and post-transmission HDR are byte-identical; the furnace remains
0.8061523 across strengths. The pre-change disabled comparison has identical
final color, visibility and G-buffer, with three HDR pixels differing by one
half-float ULP (0.00048828125), consistent with arithmetic reassociation.

TAA, spatial scaling, MetalFX and combined editor SSS/motion-blur/DoF/bloom
snapshots pass. The editor's 257×193 output has an 84×51 Scene viewport. A separate
serialized Metal API-validation resize from 513×321 to 257×193 and back passes.
These checks establish bounded integration coverage, not general temporal quality.

An actual scene bake with active surface diffusion, nested glass, ordinary blend,
thin-sheet transmission and photon caustics passes: 20,000 emitted photons,
1,550 caustic deposits, 100 valid probes and 30 valid cells. All 4,032 SH values
are finite (maximum absolute value 0.521083713). The 17,116-byte asset has SHA-256
`96d3c822b143f7554ba738cafddafe9d6a967a03a8dcfae44b902e08d1df0c0d`.
Disabling surface diffusion reproduces the preceding bake byte-for-byte, SHA-256
`62ec80db491e8b89bfcb294441ae9b0d13f31a03fe2061b4d2c2f50bc3cf8c67`.

Reproduction commands, run on 2026-09-08:

```sh
env -u VKR_DISPLAY_OUTPUT -u MTL_DEBUG_LAYER -u MTL_SHADER_VALIDATION -u VK_INSTANCE_LAYERS ./build_release.sh
python3 .scratch/build-subsurface-bsdf-check.py
python3 .scratch/build-subsurface-sampler-check.py
python3 .scratch/check-subsurface-reflection.py --generated build_release/renderer/generated/vulkan --spirv-dis /Users/Yaroslav_Panok/VulkanSDK/1.4.357.0/macOS/bin/spirv-dis --spirv-val /Users/Yaroslav_Panok/VulkanSDK/1.4.357.0/macOS/bin/spirv-val
env -u VKR_DISPLAY_OUTPUT -u MTL_DEBUG_LAYER -u MTL_SHADER_VALIDATION -u VK_INSTANCE_LAYERS ./build_release/tools/vkr_harness snapshot --case tools/cases/local/subsurface_enabled_local.case.json --profile tools/profiles/local-brdf-display-validation.json
python3 tools/checks/check_subsurface.py .scratch/subsurface-native-runs.json
./build_release/tools/vkr_diffuse_baker --scene .scratch/subsurface-bake/glass.scene.json --output .scratch/subsurface-bake/glass.vkdv --manifest .scratch/subsurface-bake/manifest.json --grid 6 4 6 --voxel-size .1 --face-size 8 --samples 4 --max-depth 12 --photons 20000 --photon-radius .3
```

Numeric results are retained in `.scratch/subsurface-profile-math.json`,
`.scratch/subsurface-endpoint-math.json`, `.scratch/subsurface-native-numeric.json`,
`.scratch/subsurface-sampler.json` and `.scratch/subsurface-bake/numeric.json`.
Native run manifests are `.scratch/subsurface-native-runs.json` and
`.scratch/subsurface-extra-runs.json`; the resize report SHA-256 is
`8a9246d16225195c6146323ddd4d060628b984568f45a2b07f3d9e2e152804e2`.
The final Release rebuild and repeated three-module reflection check pass. A
1,216-case shared-source/half-conversion oracle passes; the focused Metal
extreme-irradiance fixture keeps all 49,056 covered source pixels finite at
65,504 and preserves original/composed HDR exactly at 39,200. Its report SHA-256
is `fc9612a5e6ea534f2481d9d88315fedee3e8acdb937ea8a35cee5574f45d60c7`.
The ordinary enabled fixture and numeric checker pass again after saturation.
Results are retained in `.scratch/subsurface-half-range.json` and
`.scratch/subsurface-hdr-range-numeric.json`; final build and reflection logs are
`.scratch/renderer-improvements-subsurface-final-release.log` and
`.scratch/subsurface-final-reflection.json`. The dedicated Release editor build
also passes with `env -u VKR_DISPLAY_OUTPUT -u MTL_DEBUG_LAYER -u MTL_SHADER_VALIDATION -u VK_INSTANCE_LAYERS ./build_editor.sh Release`; its log is
`.scratch/renderer-improvements-post-effects-editor.log`. Native Vulkan execution
and bilateral comparison remain unavailable on this Mac. The feature remains UNALIGNED under
[ADR-044](044-shader-cross-backend-contract.md). No frame-cost claim is made.

## Alternatives considered

Taking a geometric mean of endpoint strengths was rejected: a half-strength
receiver next to a full-strength white surface could exceed its diffuse
allocation. Taking a geometric mean of angular reserves could similarly exceed
the grazing-angle reserve. Minimum strength and the existing receiver reserve
avoid those failures.

Finite-volume transport would require a different transport and authoring
contract. It is outside this accepted homogeneous surface approximation.

## Revisit when

Revisit the sampling or transport model if folded geometry, finite-solid
transmission, off-screen diffusion or extreme source irradiance becomes a
required scene case. Any additional storage, samples or offline transport must
settle its quality and ownership budget first.
