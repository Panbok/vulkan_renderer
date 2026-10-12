---
status: proposed
updated: 2026-10-12
authority: proposal
---

# Sparse diffuse volumes: remaining work

Sparse brick volumes shipped on 2026-10-09: placement, relocation, distance
moments, L1 layers with lamp direct bands, `DVOL` v3, the GPU composition
pass, the desktop pipeline's half-resolution lookup, the tiled pipeline's
inline lookup, the Metal probe gather, and environment reflections scaled
by baked sky light ([ADR-054](../adr/054-baked-diffuse-volumes.md)). This proposal keeps the
parts that are not implemented and the evidence still missing.

## Baseline

On Windows (RX 6700 XT, Vulkan ray-query gather) a Bistro volume at 1 m
spacing bakes:
- placement in 18.7 s;
- visibility in 87.6 s;
- each of eight sun keys in about 22 s;
- the 72-lamp group in 59 s.

Each layer traces its own paths. On the M1 Pro's Metal probe gather the same
volume takes 677 s at the default face size and samples: 40 to 44 s of GPU
time per sun key and 183 s for the lamp group.

## Proposed changes

1. **Paths shared across layers.** A path traced once adds every layer's
   lights and sky at its vertices instead of tracing nine sets of paths. On
   Bistro this would remove most of the 8 × 22 s of sun-key gathering;
   lightmap bakes would gain the same.
2. **Runtime ray-traced updates** (DDGI style) of the same bricks and
   moments on the desktop pipeline only, for dynamic lights and time of day
   without baked layers, and for blockout without a rebake. The owner chose
   this on 2026-10-12 as the desktop pipeline's dynamic diffuse GI in place
   of SSGI, which was removed on 2026-10-12. The design, open choices and evidence
   are in [Short-history rendering](short-history-rendering.md#6-ddgi-style-volume-updates-on-the-desktop-pipeline).
   The tiled pipeline stays baked.

## Evidence needed

- **Tiled pipeline.** `tiled_bistro_baked_native` before and after, with
  `Tiled.Opaque` within noise. Its indirect diffuse alone needs render mode 9
  in the harness, which accepts only five modes on Metal today.
- **Leaks.** A Bistro night capture across the café facade that shows no
  interior lamp bounce on the outer wall away from the openings.
- **Cost.** An authoritative, clean-tree timing of the desktop lookup against
  the owner's 0.5 ms budget. The local measurement in ADR-054 is 0.57 ms on
  the opaque path plus 0.10 ms in transmission shading.
