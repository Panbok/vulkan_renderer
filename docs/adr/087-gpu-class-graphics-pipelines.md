---
status: partial
updated: 2026-10-05
authority: adr
---

# ADR-087: Graphics pipelines per GPU class

## Status

Accepted (partial). The decision is in force. The tiled pipeline is not
implemented; every device, including Apple M-series, runs the current
desktop pipeline until it ships. The open design and the prototype gate are
in [Tiled graphics pipeline](../proposals/tiled-pipeline.md).

## Context

The renderer has one graphics pipeline for every supported GPU: a visibility
buffer, a compute G-buffer resolve, compute deferred lighting, four
transmission layers, screen-space effects and temporal reconstruction
([ARCHITECTURE.md](../ARCHITECTURE.md#rendering-pipeline)). Metal and Vulkan
implement the same semantics ([ADR-044](044-shader-cross-backend-contract.md)).
The supported GPUs differ in kind: Apple M1 to M4 are tile-based deferred
renderers with unified memory, RDNA 2 and Ampere are immediate-mode discrete
GPUs ([ADR-083](083-supported-hardware-matrix.md)).

The owner's targets are different per class. Discrete GPUs should go as far as
the hardware allows, including ray and path tracing. Apple M-series should run
at native resolution without upscaling at 60 fps, and its pipeline should be
the base for a later mobile renderer.

The current pipeline cannot meet the M-series target. On the M1 Pro, Bistro
with production effects at 2560×1440, render scale 1.0, portable TAA and no
upscaler took 56.12 ms mean, 52.88 ms median and 71.38 ms p95 per frame, and
the frame was GPU-bound (Release, `local-windowed-gpu-single`, one process,
120 measured frames, report digest
`sha256:1e63ae765ec38fb310c031a0a2f0abbfff50c72caae407956ee7ca9bb54b54dd`,
2026-10-05; a temporary copy of `bistro_metal_production_040` with
`render_scale` 1.0). Compute passes took 49.5 ms and raster passes 5.8 ms of
the 55.3 ms pass sum:

| Pass | GPU ms | Limiter in a GPU-counter trace of the same case |
|---|---|---|
| `Lighting.Deferred` | 12.2 | 16% occupancy, ALU 42%, 10 GB/s read |
| `Temporal.Resolve` | 10.0 (bimodal, minimum 1.8) | not attributed |
| `GBuffer.Resolve` | 7.1 | 17% occupancy, ALU 33%, 4 GB/s read, 8 GB/s write |
| `Shadow.LocalMask` | 5.9 | ALU 65% |
| `AO.Evaluate` | 4.4 | ALU 98% |

Per-pass timestamps inflate compute rows slightly
([vkr-performance](../../.codex/skills/vkr-performance/SKILL.md)). No pass is
limited by memory bandwidth; the M1 Pro provides about 200 GB/s. Keeping the
current G-buffer in tile memory would remove about 100 MB of traffic per frame,
at most about 1 ms. The cost is per-pixel shading in techniques built for
discrete GPUs. Cheaper techniques that suit tile-based GPUs, such as MSAA
resolved on chip, render-pass-local or forward shading and baked lighting,
change visible output and therefore need a separate
pipeline rather than a backend mechanism.

## Decision

1. The renderer has two graphics pipeline classes, selected by GPU
   architecture, not by graphics API:
   - The **tiled pipeline** targets tile-based GPUs: Apple M-series now, mobile
     GPUs later. It favors work that stays in tile memory, MSAA and baked or
     precomputed lighting.
   - The **desktop pipeline** targets immediate-mode discrete GPUs. It is the
     current pipeline and may add ray-traced and path-traced techniques.
   Either backend may implement either pipeline. The tiled pipeline is
   designed so that Vulkan can implement it for mobile GPUs, for example
   through dynamic-rendering local read.
2. Within one pipeline class, Metal and Vulkan share rendering semantics and
   evidence states as [ADR-044](044-shader-cross-backend-contract.md) defines.
   Parity is not required between pipeline classes.
3. Both pipeline classes share an art-level contract: the material model and
   its inputs, the color pipeline (exposure, tonemapping, display transform and
   color grading), scene and asset data, and the render graph, upload,
   residency and animation infrastructure. Lighting, global illumination,
   shadows, anti-aliasing, screen-space effects and quality presets may differ.
4. The tiled pipeline budget on the M1 Pro is Bistro at 2560×1440, render scale
   1.0, no upscaler, 16.7 ms p95 (60 fps). Quality tiers that lower M1 cost are
   allowed. Until the tiled pipeline ships, `bistro_metal_production_040` (0.4
   render scale) remains the M-series regression case for the desktop pipeline.

## Consequences

- Lighting, shadow, AA and screen-space work is implemented and validated once
  per pipeline class. Harness cases, baselines and ADR-044 evidence states are
  kept per class.
- The `AGENTS.md` rule that Metal and Vulkan share rendering semantics applies
  within a pipeline class.
- Ray and path tracing can be added to the desktop pipeline without a
  tile-based equivalent.
- The tiled pipeline may need more baked data, such as lightmaps or denser
  probes, which `vkr_bakery` owns ([ADR-077](077-asset-build-system.md)).

## Alternatives considered

- **One pipeline with quality presets.** Rejected: the measured M1 Pro cost is
  in techniques that presets can only scale, not replace.
- **Pipelines selected by API.** Rejected: a later Vulkan mobile renderer would
  need a third pipeline or a port of the Metal one.
- **Upscaling on M-series.** Rejected by the owner: native resolution is the
  target.

## Revisit when

- The tiled-pipeline prototype cannot reach the budget with the planned
  techniques.
- Apple9 dynamic register allocation changes the measured cost structure on M3
  or M4 ([ADR-083](083-supported-hardware-matrix.md)).
- A mobile target is scheduled and needs its own budget.
