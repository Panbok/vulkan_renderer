---
status: implemented
updated: 2026-09-08
authority: adr
---

# ADR-056: One-sided rectangular LTC lights

## Status

Accepted.

## Context

Punctual lights cannot represent the broad highlights of ceiling panels or
windows. Their point/spot shadow grouping and spatial masks do not describe
extended emitters. Rectangles need a separate light representation and an
explicit visibility policy.

## Decision

Support at most eight authored rectangles per scene, including disabled ones.
Reject a ninth at scene mutation and synchronous/asynchronous load boundaries.
`rectangle_light` authors `enabled`, RGB `color`, `radiance`, and full `size`
(width, height). Defaults are enabled, white, radiance one, and size (1,1).
Dimensions must be finite and positive; radiance and color finite and nonnegative.

The center is the entity's world translation. Orientation composes ancestor
and local quaternions, ignoring scale and shear; explicit dimensions are world
units. Local -Z emits, with world normal `-cross(right, up)`. Increasing area at
fixed radiance increases flux. Rectangles have no runtime shadows or attenuation
range and require authored placement that avoids unwanted light through walls.
They do not consume point/spot shadow faces. The offline baker traces visibility.

The lighting system owns a fixed eight-entry table sorted by render ID.
`VkrFrameLighting` borrows it through the frame call. Packet validation proves
count, table, dimensions, radiance and orthonormal basis before native packing.
Each native frame slot uploads a typed 64-byte row containing center/half-width,
right/half-height, up/radiance, and color. Completed slot reuse owns GPU last use.
Rectangle state participates in temporal and SSR radiance invalidation.

Two immutable 64x64 RGBA16F textures occupy 64 KiB per renderer. They contain the
inverse LTC matrix and GGX amplitude/Schlick fit from the pinned Self Shadow
correlated-Smith GGX fit. Both backends use linear clamp sampling at LOD zero.
LUT sampling and the receiver basis are prepared once per surface; the shader
then evaluates the bounded rectangle table. Horizon clipping precedes spherical
polygon integration. The normalized cosine integral is the signed edge sum
divided by 2*pi, with orientation matched to the emitting face. Specular amplitude
uses VKR's current F0, F90 and energy compensation; diffuse uses its residual
energy weight and Lambert response.

Opaque deferred, forward, and transmission reflection paths share the light
semantics and direct-light diagnostic modes. Transmission replaces diffuse as
before. No rectangles means a valid zero-count block and no lookup samples.

Native renderers own lookup creation, upload and completion-safe retirement.
Metal adds a 32-byte resource/table block through the frame pointer at byte 496;
the frame root is 512 bytes and retains the existing upload stride. Vulkan adds
its 32-byte block pointer at byte 560, making the frame root 576 bytes. This does
not change the unrelated 560-byte Vulkan utility root. Vulkan reserves two permanent
sampled-image slots and reuses the DFG sampler. Staging becomes retired only after
the upload is submitted; cancellation keeps initialization pending for a later
submission. No new graph pass or image is required.

The CPU baker samples uniform rectangle area for next-event estimation, converts
the area PDF to solid angle, and uses its existing BVH/transmittance segment for
visibility, including blended and refractive surfaces. Photons start uniformly
on the rectangle with cosine-weighted front-hemisphere directions. Emitted flux
is `pi * area * color * radiance`, with light-selection compensation. The existing
multi-bounce and caustic transport then consumes those photons.

## Consequences

Runtime rectangles improve extended-light response without adding shadow passes.
LTC remains a fitted BRDF approximation, and unshadowed runtime light can cross
walls. The baker's visibility and transport are more complete than runtime direct
visibility. Native Vulkan execution and bilateral image comparison are unavailable
on the current macOS host; compilation does not establish that parity.

The eight-light affine cost is not an accepted performance result. One
non-authoritative deferred measurement recorded GPU time of 10.664609 ms with
eight lights and 4.658021 ms without, and wall-clock p50 of 23.409458 ms and
16.934666 ms respectively. That roughly 6 ms difference is a limitation of the
measured configuration, not a speed claim or a cross-backend budget.

## Evidence and remaining checks

Metal evaluates the 12-pixel GGX quadrature fixture after the affine update with
maximum error `0.001048130738` (`.scratch/ltc-affine-numeric.log`). No-light and
back-face cases preserve the prior HDR exactly. The radiance/8 scaling and layered
forward, deferred and transmission paths pass, as does Metal API validation. Scene
mutation and synchronous/asynchronous loaders reject a ninth authored rectangle.

The baker's opaque rectangle fixture has 27 valid probes and emits 2,000 photons.
Its glass fixture has 100 valid probes; 20,000 photons produce 3,643 caustic
deposits. The full CPU suite passed (`.scratch/ltc-cpu-verified`). The final
shared affine and fast-path Release build passed, and the front-facing pre-fast-path
and first fast-path outputs are byte-identical.

The Metal API result remains applicable because the lookup-resource path did not
change. A later run stopped before LTC execution because an older executable did
not recognize a newer fog graph condition; it is not an LTC failure. Compiled
Vulkan SPIR-V reflection confirms the 576-byte frame root, 32-byte LTC block and
64-byte row. Native Vulkan execution remains unavailable on this macOS host.

## Alternatives considered

Reusing point/spot rows conflates emitter geometry with shadow-face policy.
A center-point shadow would only approximate area visibility and needs its own
quality contract. Fixed-flux authoring would change brightness when resizing;
emitted radiance keeps runtime integration and baker flux in the same units.

## Revisit when

Scenes require more than eight rectangles, runtime area shadows, textured or
two-sided emitters, or measured frame cost calls for spatial light selection.

## Implementation

- [Light data and validation](../../renderer/src/vkr_lighting.h)
- [Shared LTC integration](../../renderer/src/shaders/shared/ltc_kernel.slangh)
- [Lookup data and license](../../renderer/src/vkr_ltc_lut.c)
- [Scene lighting owner](../../runtime/src/renderer/systems/vkr_lighting_system.c)
- [Baker transport](../../tools/bake/vkr_bake_integrator.cpp)
- [Metal packet ABI](../../renderer/src/metal/vkr_metal_packet_abi.h) and [Metal Slang draw root](../../renderer/src/shaders/metal/slang/common/draw.slangh)

Regenerate the half tables using `python3 tools/generate_ltc_lut.py --source
<ltc.js>`. The converter rejects source bytes that differ from Self Shadow commit
`31e5e96b54f98f33098f8503003119ba2231a1c6`, `fit/results/ltc.js`, SHA-256
`21071160163defd419b8f754ab604a8e60c44fcf539bfff9916e0e8e82316c1d`.
