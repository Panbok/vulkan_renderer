---
status: implemented
updated: 2026-09-05
authority: adr
---

# ADR-038: GPU-resident L2 diffuse response

## Status

Accepted implementation; retained quality, lifetime and performance acceptance remains open.

## Context

Diffuse environment response is low frequency, while specular reflection needs
directional mipmapped radiance. Replacing diffuse storage must preserve its
existing radiometric normalization and submitted readers.

## Decision

Store nine RGB L2 coefficients for normalized diffuse response `D(n) = E(n)/pi`.
Projection weights exact source texels by solid angle and folds band factors
`1`, `2/3`, `1/4` into storage. A constant radiance source evaluates to that
radiance; shading does not add another division by pi. Authored deringing
windows higher bands; evaluation clamps negative reconstructed output.

Keep coefficients GPU-resident in a copy-on-write pool: immutable black slot 0
plus 36 reusable slots for two generations of the fallback, active environment
and 16 probes. Reserve, record, submit-publish, last-reader tracking, retire and
collect are distinct states. A failed unsubmitted projection abandons its slot.
Exhaustion reports a cold-path error and preserves the prior publication or black;
it cannot become a successful-frame wait or overwrite.

Packets carry the coefficient buffer and slot identities, not coefficient values.
Metal loads source cube texels; Vulkan uses a lazily published 2D-array alias for
exact texel access. Source, skybox and GGX specular prefilter remain cubemaps under
ADR-016. The former diffuse cubemap and its A/B runtime path are removed.

## Consequences

Diffuse storage and source integration are implemented on both backends. The
representation is approximate and requires scene quality review; it does not
supply geometric occlusion. Remaining acceptance covers deterministic GPU
projection fixtures, probe quality, submitted-frame reload/lifetime stress and
matched performance. Final-path timings alone cannot reconstruct a removed
same-binary cubemap control.

## Alternatives considered

A baked diffuse cubemap retains a larger sampled representation. CPU-owned
coefficients require a different preparation/readback owner. In-place updates
would overwrite data still consumed by submitted frames.

## Revisit when

Quality fixtures exceed the accepted L2 error, or measured probe scale justifies
another representation without weakening normalization or lifetime.

## Implementation

[`vkr_ibl_math.h`](../../lib/src/renderer/vkr_ibl_math.h),
[`vkr_ibl_sh_pool.c`](../../lib/src/renderer/vkr_ibl_sh_pool.c),
[`sh_l2_kernel.slangh`](../../lib/src/renderer/shaders/shared/sh_l2_kernel.slangh),
[`sh_projection.metal`](../../lib/src/renderer/shaders/metal/msl/ibl/sh_projection.metal), and
[`vkr_vulkan_ibl.c`](../../lib/src/renderer/vulkan/vkr_vulkan_ibl.c).
