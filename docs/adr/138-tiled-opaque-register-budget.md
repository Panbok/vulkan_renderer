---
status: implemented
updated: 2026-10-30
authority: adr
---

# ADR-138: Tiled opaque pass register budget

## Status

Accepted. The tiled pipeline's forward fragment shading
([tiled.metal](../../renderer/src/shaders/metal/msl/world/tiled.metal),
[lighting.metalh](../../renderer/src/shaders/metal/msl/world/lighting.metalh),
[sampling.metalh](../../renderer/src/shaders/metal/msl/shadow/sampling.metalh))
is ordered and typed to bound its register count, and frames without a
diffuse volume take shading variants that compile its lookup out. Metal
only; the desktop pipeline (ADR-087) does not run this code.

## Context

On 2026-10-29 GPU counters over `Tiled.Opaque` on the Bistro street view
(2560×1440, MetalFX at 0.6, M1 Pro) read fragment occupancy 17%, ALU limiter
44%, 16-bit ALU use 0% and the cache limiters at 3 to 10%: latency-bound at
low occupancy ([ADR-129](129-tiled-standard-draw-kinds.md)). Every forward
variant then used 104 to 108 32-bit registers per thread, with or without
dynamic lights. Three parts of the shader each reached 88 to 104 registers
with the rest of the shading live: the sun's cascade filter (16 taps and the
PCSS search issued together), the shadowed light loop, and the diffuse
volume and environment tail.

Register counts come from the Metal Toolchain offline: `xcrun metal` builds
the library source with the `metal_stdlib` preamble the runtime compiler
supplies, and `applegpu-nt -arch applegpu_g13s -N <script>.mtlp-json`
translates one render pipeline per specialized fragment (function constants
set as the host sets them) to M1 Pro binaries. The fragment's
`__GPU_METADATA` table holds the registers per thread; shaders with a known
number of live values (4 to 96, read as 8 to 108 registers) calibrated the
field. On the lightmapped Bistro, fewer registers gave proportionally higher
fragment occupancy down to about 80: 104 registers 19.5%, 88 22.8%, 80
25.0%, 44 35.0%.

## Decision

1. **The sun's cascades first.** `vkr_metal_tiled_early_sun_visibility`
   samples the cascades and the cloud shadow before the material is read,
   gated on the geometric normal facing the sun. A surface whose geometric
   normal faces away lies in its own shadow in the cascades.
2. **The diffuse volume per probe.** `vkr_metal_packet_diffuse_volume`
   evaluates each probe's SH at the normal as it reads the probe, so the
   eight probes' coefficients are never live together.
3. **The cloud chain as a cube.** The cloud-lit sky light's chain is a
   per-command-slot RGBA16F cube texture sampled with hardware filtering
   ([ADR-074](074-volumetric-cloud-layer.md) owns its contract).
4. **Shadowed local lights.** A shadowed light reads its offset, depth
   range and projection from its first shadow view. A point light's face is
   projected from the offset along the face's axes, each a signed component
   of the offset (`vkr_metal_local_shadow_face_offset`), with no face matrix. Lights below
   `VKR_LOCAL_LIGHT_CONTRIBUTION_CUTOFF` take no shading, as in the desktop
   pipeline's deferred lighting.
5. **No-volume variants.** Frames whose scene binds no diffuse volume take
   `VKR_METAL_TILED_NO_VOLUME_VARIANT` (function constant 8), whose forward
   pipelines compile the volume lookup out. They are created in the
   background like the other shading variants
   (`vkr_metal_packet_tiled_no_volume_frame`); `VKR_TILED_NO_VOLUME_DISABLED`
   keeps every frame on the general pipelines for diagnosis.
6. **Half-precision light totals.** `VkrMetalTiledSurfaceLight` keeps the
   diffuse, specular and emissive totals in `half3`, the precision the
   colour target stores. `vkr_metal_packet_punctual_layered` accumulates in
   the caller's `Sum` type. Per-light terms stay in single precision: the GGX
   distribution exceeds the half range on glossy surfaces.
7. **Lobes before the shadow lookup.** The punctual loop evaluates a light's
   diffuse and specular lobes unshadowed, then multiplies them by the
   light's visibility. Across the shadow lookup only the two lobes stay
   live, not the light's direction, colour and falloff.

A change to the forward shading counts the registers of its affected
variants before and after, and keeps a change that raises them only with a
measured gain.

## Consequences

Measured with 2 s Metal System Traces attached to a harness child, on the
lightmapped Bistro (`bistro_tiled_local`) and on Bistro with dynamic lamps
(`bistro.scene.json`), static street view, 2560×1440, MetalFX at 0.6, M1 Pro,
Release, dirty tree, local and not authoritative:

| Change | Registers | Lightmapped Bistro | Bistro, dynamic lamps |
|---|---|---|---|
| Before | 104 to 108 | 3.84 ms, 19.5% occupancy | 7.42 / 7.57 ms, 19.0% |
| 1 and 2 | | 3.52 ms, 21.7% | 7.28 ms, 19.8% |
| 3 | | 2.29 ms, 20.9% | 5.90 ms, 19.9% |
| 4, first view and cutoff | | 2.28 ms | 5.64 ms |
| 4, face projection; 5 | shadowed loop 88 → 80; general no-volume 92 → 76 | 2.10 ms | 5.64 ms |
| 6 | every variant −4 | 1.97 ms, 25.5% | 5.64 / 5.65 ms |
| 7 | shadowed no-volume 84 → 80 | 1.96 / 1.98 ms | 5.32 / 5.33 / 5.34 ms, 23.1% |
| 4, signed components in place of axis dot products | unchanged, 264 bytes less code | | 5.40 / 5.41 ms against 5.54 / 5.56 ms in the same batch |

Captures against the build before each change
(`tiled_bistro_capture`, `tiled_bistro_baked_capture`, `local-offscreen`
profile):

- Rows 1 to 3: 71.9 dB, at most 3 to 8 of 255 on 113 pixels or fewer.
- Row 4, first view and cutoff: lightmapped Bistro identical, Bistro with
  dynamic lamps at most 1 of 255.
- Face projection: 6 pixels of Bistro with dynamic lamps move by at most
  7 of 255 (76.4 dB), from rounding at shadow edges.
- No-volume variants: equal to the same binary's captures with
  `VKR_TILED_NO_VOLUME_DISABLED`.
- Row 6: at most 2 of 255 (65 to 67 dB).
- Row 7: lightmapped Bistro identical, Bistro with dynamic lamps at most 1 of
  255 (73.3 dB).
- Signed components: Bistro with dynamic lamps identical.

After row 7, counters over `Tiled.Opaque` on Bistro with dynamic lamps read
fragment occupancy 23.1%, ALU limiter 62%, F32 utilization 25.5% and F16
utilization 0.1%. Removing parts of the light loop (lights 4.06 ms of
5.64 ms) attributed about 0.24 ms to the list walk, 1.35 ms to the light row
loads, 0.3 ms to the falloff terms, 0.92 ms to the lobes and 1.33 ms to the
local shadows. At that view (1536×864 internal), a fragment walks 12.5 light
entries on average (p90 23), finds 7.5 in range and shades 7.2, 3.0 of them
shadowed, from a capture whose forward pass wrote those counts to
`hdr_pre_bloom`. The remaining cost is the number of shaded lights per
pixel, near the ALU limit. On M1 16-bit arithmetic runs at the 32-bit rate,
so half precision helps only where it frees registers.

Vulkan does not run this code; no desktop evidence applies.

## Alternatives considered

Each was built or compiled on 2026-10-29 or 2026-10-30 with the same
measurement and not kept:

- **Deferred local lights.** A full-screen draw at the end of each opaque
  pass reading the surface from four memoryless targets. The forward draws
  without lights still took 92 registers, the light draw 64 with unshadowed
  lights, 80 with shadowed ones and 116 with rectangle lights. That would
  raise occupancy by about a seventh for the lights' 3.6 ms, against four
  more targets (the texture demand target alone costs about 1 ms) and a
  costlier draw for rectangle lights.
- **Sun cascade taps shared across each 2×2 quad.** Each fragment read the
  quarter of the 16 grid taps and 8 blocker taps toward its corner of the
  quad, 6 taps instead of 24. Time stayed inside the spread (1.96 to 2.26
  against 1.97 to 2.28 ms lightmapped, 5.62 against 5.64 to 5.65 ms with
  lamps): sampled first, the taps no longer set the register count. The
  penumbra would have become constant across each quad.
- **Culling each SIMD-group's candidate lights against the box around its
  fragments**: 5.55 to 5.79 ms against 5.64 ms, with 4 more registers;
  60% of the walked entries are already in range.
- **The tile's first 64 light rows copied to threadgroup memory** by the
  tile kernel: 5.91 to 6.14 ms against 5.64 ms.
- **Light rows in the `constant` address space**: 5.77 to 5.97 ms.
- **A SIMD-uniform light index** (`simd_broadcast_first`): 5.84 to
  6.05 ms.
- **Half-precision lobes inside the loop** with the specular distribution
  kept in single precision: 4 more registers.
- **A higher contribution cutoff** (0.004, 0.01): 5.31 to 5.48 ms against
  5.32 ms. A light still runs for its whole SIMD-group when any fragment
  needs it, so skipping faint fragments saves nothing.
- **Diffuse and environment light ahead of the direct light, or the
  lightmap and volume block ahead of it**: 4 more registers. A volume loop
  unroll pragma changed nothing.

## Revisit when

A shading change adds a part that is live across the light loop or the
volume tail; the number of shaded lights per pixel rises past the Bistro
street's 7; a GPU family with a faster 16-bit rate or a different register
file becomes a tiled target; or the owner accepts an image change that
bounds the lights shaded per pixel.
