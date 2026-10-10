---
status: proposed
updated: 2026-10-09
authority: proposal
---

# Sparse diffuse volumes: remaining work

Sparse brick volumes shipped on 2026-10-09: placement, relocation, distance
moments, L1 layers with lamp direct bands, `DVOL` v3, the GPU composition
pass and the desktop pipeline's half-resolution lookup
([ADR-054](../adr/054-baked-diffuse-volumes.md)). This proposal keeps the
parts that are not implemented and the evidence still missing.

## Baseline

On Windows (RX 6700 XT, Vulkan ray-query gather) a Bistro volume at 1 m
spacing bakes:
- placement in 18.7 s;
- visibility in 87.6 s;
- each of eight sun keys in about 22 s;
- the 72-lamp group in 59 s.

Each layer traces its own paths. A Mac bakes volumes on the CPU only,
because Metal has no probe gather. The tiled pipeline's composition kernel
and inline lookup are written, but have not been compiled or run.

## Proposed changes

1. **Paths shared across layers.** A path traced once adds every layer's
   lights and sky at its vertices instead of tracing nine sets of paths. On
   Bistro this would remove most of the 8 × 22 s of sun-key gathering;
   lightmap bakes would gain the same.
2. **Metal probe gather.** Port the Vulkan kernel's probe gather
   (`probe_gather` in
   [`vkr_bake_lightmap.slang`](../../tools/bake/vkr_bake_lightmap.slang)) to
   [`vkr_bake_metal.mm`](../../tools/bake/vkr_bake_metal.mm), so a Mac stops
   falling back to the CPU integrator.
3. **SSGI from unbaked light.** Let SSGI add dynamic lights and the emission
   of dynamic objects on covered pixels without counting baked light twice;
   ADR-060 names this revisit.
4. **Specular occlusion from the volume.** Scale environment specular by the
   ratio of volume to environment irradiance, so glossy surfaces inside the
   café stop reflecting the sky.
5. **Runtime ray-traced updates** of the same bricks and moments on the
   desktop pipeline, for dynamic lights and time of day without baked
   layers.

## Evidence needed

- **Tiled pipeline.** `tiled_bistro_baked_native` before and after, with
  `Tiled.Opaque` within noise, and a Metal capture of a draw without a
  lightmap lit by the volume.
- **M1 Pro.** Bake time and peak memory for the CPU and, after item 2, the
  Metal gather.
- **Leaks.** A Bistro night capture across the café facade that shows no
  interior lamp bounce on the outer wall away from the openings.
- **Cost.** An authoritative, clean-tree timing of the desktop lookup against
  the owner's 0.5 ms budget. The local measurement in ADR-054 is 0.57 ms on
  the opaque path plus 0.10 ms in transmission shading.
