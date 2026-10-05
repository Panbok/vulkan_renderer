---
status: partial
updated: 2026-10-06
authority: adr
---

# ADR-087: Graphics pipelines per GPU class

## Status

Accepted (partial). The decision is in force. A first tiled pipeline runs on
Metal when a renderer or the editor selects it (decisions 6, 7 and 9) and
draws glass (decision 10); every device runs the desktop pipeline by default
until the tiled one draws dynamic lights. The remaining design is in
[Tiled graphics pipeline](../proposals/tiled-pipeline.md).

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
5. The tiled pipeline shades opaque surfaces forward, after a depth pre-pass,
   in one multisampled render pass (owner decision 2026-10-05). On Bistro
   it costs the same as a G-buffer kept in tile memory without multisampling
   and 1.7 to 3.4 ms less with four samples
   ([measurements](../proposals/tiled-pipeline.md#first-prototype-measurements)).

6. The tiled pipeline has its own render graph,
   [`tiled.rendergraph.json`](../../assets/render_graphs/tiled.rendergraph.json),
   and keeps the desktop graph's culling, cascades, sky, cloud and post
   passes. One graph pass, `Tiled.Opaque` (`pass.tiled.opaque`), owns the
   depth pre-pass, forward shading and the clear sky in one render pass whose
   colour and depth are memoryless four-sample targets the Metal backend
   owns; they resolve on chip into the graph's `hdr_scene_color` and
   `opaque_vbuffer_depth`, depth to the nearest sample. The graph compiler is
   unchanged. The cloud trace needs that depth, so `Tiled.Clouds`
   (`pass.tiled.clouds`) lays the cloud layer and the discs it lets through
   over the resolved sky pixels afterwards; edge pixels mixing sky and surface
   samples miss the clouds.
7. `VkrRendererBackendConfig.graphics_pipeline` selects the class
   (`VKR_GRAPHICS_PIPELINE=desktop|tiled` overrides it, and harness cases set
   `renderer.graphics_pipeline`). Zero is the desktop pipeline. The tiled
   class requires the Metal backend without temporal upscaling or dynamic
   resolution; it turns off temporal reconstruction, SSR, SSGI, GTAO,
   surface diffusion, depth of field, motion blur, froxel fog, local shadows,
   SDSM and the transmission passes
   ([`vkr_renderer.c`](../../renderer/src/vkr_renderer.c),
   [`vkr_render_graph_frame.c`](../../renderer/src/vkr_render_graph_frame.c)).
8. The forward shader
   ([`tiled.metal`](../../renderer/src/shaders/metal/msl/world/tiled.metal))
   uses the shared material model: filtered roughness, the DFG energy, the
   shared direct term for the sun with its cascades and cloud shadow, and
   environment specular with horizon and specular occlusion. Diffuse light
   comes from the draw's lightmap
   ([ADR-088](088-baked-lightmap-sets.md)): the frame's active layers, each
   weighed by the sun and its light group; a draw without one takes the baked
   diffuse volume, else the global environment. Clearcoat, sheen, anisotropy,
   diffuse transmission, IBL probes and punctual and rectangle lights are not
   drawn yet. The vertex stage is MSL, because the Slang module numbers entry
   point buffers in declaration order and the GPU-encoded commands bind the
   draw root at buffer 0.
9. The editor runs the tiled pipeline. The tiled graph carries the editor's
   Scene image passes (resolve, grid, selection mask and outline, overlay
   and composite), which read the resolved `hdr_scene_color` and
   `opaque_vbuffer_depth`. Without a visibility buffer, a pick replays the
   opaque GPU-driven draws into the picking target (`Picking.Tiled`,
   `pass.picking.tiled`); picks are rare, so frames without one pay nothing,
   and an alpha-tested surface picks whole. The pick readback also copies the
   picked pixel's resolved depth, which grid fit reads in scenes without
   collision. Preferences ▸ Graphics ▸ Tiled pipeline and `gfx.tiled`
   ([ADR-075](075-editor-cmd-bar-and-evaluator.md)) select the class for the
   next start. A project-managed editor starts from a machine-local
   `graphics.json` beside the workspace locator
   ([ADR-069](069-editor-projects-and-workspaces.md)), and a project's stored
   restart-time settings do not override it
   ([`vkr_graphics_settings_keep_restart`](../../runtime/src/vkr_graphics_settings.h)).
10. The tiled pipeline draws glass with the alpha-blended surfaces. The
    runtime puts transmissive draws in the camera-culled, back-to-front blend
    list (`transmission_blended` in
    [`vkr_scene_build_world_draws`](../../runtime/src/renderer/systems/vkr_scene_frame.h)),
    and `Tiled.Blend` (`pass.tiled.blend`) draws that list single-sampled
    over the resolved image after the clouds, then world text. Glass
    composes as the transmission passes do (`vkr_transmission_compose`), but
    the light behind it arrives through dual-source blending: the fragment's
    second output is the factor the destination keeps per channel, so tinted
    and stacked panes compose in draw order. All glass draws as thin, smooth
    glass, without a refraction offset, rough blur, volume attenuation or
    transmission and thickness textures; Bistro's 18 glass materials have no
    thickness and an effective roughness of zero. The pass runs only on
    frames with blended draws or world text, because its load and store of
    the resolved colour and depth cost about 0.4 ms at 2560×1440.

### First tiled pipeline measurement

Release, M1 Pro, 2026-10-06: the production Bistro orbit at 2560×1440 pixels,
render scale 1.0, 120 warm-up and 300 measured frames, one process per run,
`./build_release/tools/vkr_harness profile --case
tools/cases/local/tiled_bistro_native.case.json --profile
tools/profiles/local-windowed-gpu-submission-single.json` and its desktop
counterpart `tiled_bistro_native_desktop`, run alternately twice:

| Pipeline | `gpu.submission` median | p95 | Reports |
|---|---|---|---|
| Desktop | 54.27 / 54.34 ms | 69.06 / 69.03 ms | `sha256:52045c82ae920695d2b6c330f612b2f2c849c36f1f33559fb4558cee4c8ac3f3`, `sha256:9478cf49688c75b32e835190940fb01124cb2331c6d72d6846324a67e1e1f090` |
| Tiled | 13.25 / 13.23 ms | 16.81 / 16.76 ms | `sha256:9be628556e916fa02f684e2bedcaa9f5aefdded56db17a9271d16f92ba4924df`, `sha256:35342b3c425b3295cef3b2c0730c4c3eb7cb2e5114a65ccddbb135be4dccf0e4` |

The work differs: the tiled frame draws no transmission, local lights or
local shadows, and Bistro has no lightmap set, so its lamps add no light. The
p95 sits 0.1 ms above the budget before those return. With pass timestamps
(`local-windowed-gpu-single`,
`sha256:a8f07f292013cecff3cdbce2fd45347bdbf6648ea970c86a811ce838d0578a74`),
`Tiled.Opaque` takes 8.1 ms median (11.6 ms p95), a re-rendered cascade 1.4
to 2.1 ms, tonemapping with its display-linear cache 1.75 ms, and the cloud
trace and cloud draw 0.35 and 0.32 ms. By inspection, street-view captures
of both pipelines (`tiled_bistro_capture`, `tiled_bistro_capture_desktop`)
match in sky and sunlit surfaces; the tiled one lacks lamp light, glass and
ambient occlusion.

### Glass evidence

Release, M1 Pro, 2026-10-06: the Bistro orbit with pass timestamps
(`tiled_bistro_native`, `local-windowed-gpu-single`, report
`sha256:88d5f93cdc606960a49341ae3d0622b7b4cefa3f9561aff1428b9054edaf26a9`)
draws 325 blended draws, all glass, and `Tiled.Blend` takes 0.40 ms median
and 0.60 ms p95. Captures of the café windows on both pipelines
(`tiled_bistro_glass`, `tiled_bistro_glass_desktop`, reports
`sha256:e526a05950f7929f62d2eff55c7980e1596b24637d0ebcd2cc8d6109fd2f5599`
and `sha256:a665f9223234785166e38502f335266702e156483cb1f6f4db4aa1c77d42b571`)
show the interior through the panes on both. The tiled panes reflect the
sky where the desktop ones reflect the street, because the tiled pipeline
has no local reflection probes or SSR yet, and the desktop interior is lit
by lamps the tiled one does not draw yet.

### Editor evidence

Release editor, M1 Pro, 2026-10-06, isolated `HOME`. On the toolkit test
level, a headless run setting `gfx.tiled = true` wrote the machine-local
`graphics.json`; the next start ran the tiled pipeline with `gfx.restart`
false after the project opened, a click picked the floor and its outline,
grid and labels drew. On Bistro, which has no collision, `camera.view top;
grid.fit` read the picked depth as 16.8432 m on the tiled pipeline and
16.8431 m on the desktop one. The CPU test `test_tiled_graph_topology`
compiles an editor frame that picks.

## Consequences

- Lighting, shadow, AA and screen-space work is implemented and validated once
  per pipeline class. Harness cases, baselines and ADR-044 evidence states are
  kept per class.
- The `AGENTS.md` rule that Metal and Vulkan share rendering semantics applies
  within a pipeline class.
- Ray and path tracing can be added to the desktop pipeline without a
  tile-based equivalent.
- The tiled pipeline needs more baked data, which `vkr_bakery` owns
  ([ADR-077](077-asset-build-system.md)): lightmap sets
  ([ADR-088](088-baked-lightmap-sets.md)) and possibly denser probes.

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
