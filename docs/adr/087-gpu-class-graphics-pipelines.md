---
status: partial
updated: 2026-10-06
authority: adr
---

# ADR-087: Graphics pipelines per GPU class

## Status

Accepted (partial). The decision is in force. A first tiled pipeline runs on
Metal when a renderer or the editor selects it (decisions 6, 7 and 9), draws
glass (decision 10) and a bounded set of dynamic lights (decision 11). On a
lightmap-baked Bistro it takes 18.5 ms p95, 1.8 ms over the budget (see
[Baked Bistro measurement](#baked-bistro-measurement)); every device runs the
desktop pipeline by default until the tiled one meets it. The remaining design is in
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
   surface diffusion, depth of field, motion blur, froxel fog, the local
   shadow mask, SDSM and the transmission passes
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
11. The tiled pipeline lights static lights only through baked data, the
    lightmaps and diffuse volumes, and draws a bounded set of dynamic lights
    forward. With `static_lights_baked`, which the runtime sets for the tiled
    class, the lighting system leaves static lights out of its tables; then
    `vkr_lighting_system_limit_point_lights`
    ([`vkr_lighting_system.h`](../../runtime/src/renderer/systems/vkr_lighting_system.h))
    keeps the 16 dynamic point and spot lights nearest the camera by the
    distance to their range, and lets the 4 nearest shadow casters among
    them cast shadows. The forward shader evaluates them through the shared
    light grid and local-light loop (`vkr_metal_packet_punctual_layered`),
    and dynamic rectangle lights through the shared LTC path. A shadowed
    light takes one bilinear comparison of its local shadow map
    (`vkr_metal_packet_local_shadow_sample<false, false>` in
    [`sampling.metalh`](../../renderer/src/shaders/metal/msl/shadow/sampling.metalh))
    instead of the desktop Poisson disk, so its shadow has no soft penumbra
    and shows shadow-map texels at the edge (owner decision, 2026-10-06).
    The tiled graph renders the local shadow atlas with the desktop passes
    `Shadow.Local.Clear` and `Shadow.Local`; glass casts no local shadow, so
    the sampler reads no refractive layers. A static light in an unbaked
    scene adds no light. Each frame shades with the fragment variant for its
    dynamic lights (`VkrMetalTiledLighting` in
    [`tiled.metal`](../../renderer/src/shaders/metal/msl/world/tiled.metal)
    and `vkr_metal_packet_tiled_lighting`): none, point and spot lights
    without shadows, with shadows, or rectangle lights as well. Light code a
    frame does not use still costs `Tiled.Opaque` its registers with no light
    in range, on Bistro 0.65 ms median for the light loop, 1.2 ms for the
    former shadow filter and 1.1 ms for the rectangle-light path.

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

### Dynamic light evidence

Release editor, M1 Pro, 2026-10-06, the toolkit test level at 22:00: a
dynamic spot light casting shadows over a brush pillar and an unshadowed
dynamic point light, added through `vkr_mcp`, light the room on both
pipelines with the same pool, pillar shadow and tint, over the baked lamp
groups on the tiled one. `test_point_light_limit_keeps_nearest` and the
time-of-day lighting test cover the limit and the static-light filter.

### Editor evidence

Release editor, M1 Pro, 2026-10-06, isolated `HOME`. On the toolkit test
level, a headless run setting `gfx.tiled = true` wrote the machine-local
`graphics.json`; the next start ran the tiled pipeline with `gfx.restart`
false after the project opened, a click picked the floor and its outline,
grid and labels drew. On Bistro, which has no collision, `camera.view top;
grid.fit` read the picked depth as 16.8432 m on the tiled pipeline and
16.8431 m on the desktop one. The CPU test `test_tiled_graph_topology`
compiles an editor frame that picks.

### Baked Bistro measurement

Release, M1 Pro, 2026-10-06: the production Bistro orbit at 2560×1440, render
scale 1.0, 120 warm-up and 300 measured frames, one process per run,
`local-windowed-gpu-submission-single`, the three cases run alternately twice.
The scene is the fixture
[`bistro_tiled_local`](../../assets/scenes/fixtures/bistro_tiled_local.scene.json):
Bistro cooked with lightmap UVs and its 16-sample lightmap set from the bake
recorded in [ADR-088](088-baked-lightmap-sets.md#evidence); the sets baked
since have the same pages, layers and format. The dynamic variant,
[`bistro_tiled_dynamic_lights_local`](../../assets/scenes/fixtures/bistro_tiled_dynamic_lights_local.scene.json),
adds 16 dynamic lights near the orbit centre, 4 shadowed spot lights and 12
point lights. The desktop pipeline ignores the set and lights the model's
static lamps at runtime.

| Case | `gpu.submission` median | p95 | Reports |
|---|---|---|---|
| `tiled_bistro_baked_native` | 14.49 / 14.46 ms | 18.60 / 18.49 ms | `sha256:8b6eb2b5d7e0deedced8148b7fe9342605a1c2035ac827a5a35d80961632955c`, `sha256:6f562f20b182258a08fd0e485500eba196a8a189d38174776a96b5af44f11d47` |
| `tiled_bistro_baked_dynamic_native` | 18.47 / 18.52 ms | 28.75 / 28.65 ms | `sha256:2a68b2cf854f0e0a7e508a47dfc033449592f4b0ecd28b433146458c565b98ba`, `sha256:4866e1037fd267a5a481f68f752fa1d3db61cdbb99095e19fe3ad20accb86a5a` |
| `tiled_bistro_baked_native_desktop` | 53.91 / 53.97 ms | 68.35 / 68.26 ms | `sha256:640ff3fa474ed74a7ffae1dca91b105c86c1c67325fdc79e2871f63acc8f5785`, `sha256:f24a324dad57ed7810c1a72b53dffd9540bd4489e959aea366b8ffcc93dae0e1` |

The baked tiled frame misses the 16.7 ms p95 budget by 1.8 ms. With pass
timestamps (`local-windowed-gpu-single`), `Tiled.Opaque` takes 8.88 ms
median and 12.68 ms p95
(`sha256:d19cc35f8121b66703e7c264b7f6aa58b3e1fa5f6921bf8975a080d01f71869f`)
against 8.06 and 11.48 ms for `tiled_bistro_native`, the model without
lightmap UVs or a set
(`sha256:57124e7e7cf50bca5649a93025147d48f3206b5009d106bda0151ddfd8352972`):
the extra vertices of the lightmap-UV seams and the three active layers
cost about 0.8 ms median and 1.2 ms p95, and `Tiled.Blend` 0.43 and 0.63 ms.
The p95 frames add a re-rendered cascade, 1.3 to 2.2 ms, to the largest
opaque cost. Sixteen dynamic lights raise `Tiled.Opaque` to 12.71 ms median
and 22.71 ms p95
(`sha256:0c972501f6e4f479e7465aa475e0dcb823b95d1a5589b3ba4fab461dd714101c`);
their four shadow faces stay cached, so the cost is the per-pixel light
loop and its inline shadow filtering, and that tier does not fit the budget.
The shading variants of decision 11 lower that cost; see
[Light shading variants](#light-shading-variants).

### Light shading variants

Release, M1 Pro, 2026-10-06, the cases and settings above. HEAD is commit
`b34e8ee3`, whose one variant with lights carried the light loop, the
Poisson shadow filter with refractive layers and the rectangle-light path.
Each build ran its cases in one session, the new build first, ten minutes
apart. `zz_tmp_dyn_unshadowed` and `zz_tmp_dyn_rect` were temporary copies of
the dynamic case: the same 16 lights with `casts_shadow` false, and the 16
lights with one dynamic 2×1 m rectangle light at the orbit centre.

`gpu.submission`, `local-windowed-gpu-submission-single`, two runs each:

| Case | HEAD median | HEAD p95 | New median | New p95 |
|---|---|---|---|---|
| `tiled_bistro_baked_native` | 14.66 / 14.59 ms | 19.17 / 19.29 ms | 15.02 / 14.67 ms | 19.94 / 19.04 ms |
| `tiled_bistro_baked_dynamic_native` | 18.70 / 18.76 ms | 30.27 / 29.37 ms | 16.59 / 16.57 ms | 24.45 / 24.55 ms |

HEAD reports:
`sha256:4be20ae9d0653b1f101e3abaa00932393158c9afbb2b56f8b42ed54bbcf8c2d8`,
`sha256:c43d1fda944a2ff98e8f768f1ff1c4ad9fd788ffbee1b1f55a7c3cf01d90813f`,
`sha256:b1b437d5a74dcd264591934b4b4d37145a9684d729a1962d7e0226db797de904`,
`sha256:bf10df4585b3762cde3a4506d95ac1ec023f64e24a2086e4f8396dfb47cd0cb4`.
New reports:
`sha256:7b85145ad41c9f5a95a0124100985c1af2a5de8c6d8e5e31cf97854a9229ff49`,
`sha256:aa80cf19c7076024c4d0214d92ec6aa3f69a1c6eed5be1855aeac218913c2e17`,
`sha256:dd469cb270a8ec60bdb7b326eb1b904ec305b2bd220e1288b2e52cb2e1ac6731`,
`sha256:60243c90c4ac6fbabfb8fc32586ad588114a34db78810a1868cfe9b216c58f78`.

`Tiled.Opaque` with pass timestamps, `local-windowed-gpu-single`, one run
each for HEAD and two for the new build:

| Case | HEAD median / p95 | New median / p95 |
|---|---|---|
| `tiled_bistro_baked_native` | 9.04 / 12.86 ms | 8.94 / 13.01 ms |
| `zz_tmp_dyn_unshadowed` | 13.13 / 22.36 ms | 10.38 / 16.87, 10.25 / 17.48 ms |
| `tiled_bistro_baked_dynamic_native` | 12.99 / 22.94 ms | 10.89 / 18.15, 10.73 / 18.21 ms |
| `zz_tmp_dyn_rect` | 15.48 / 29.29 ms | 15.53 / 30.98, 15.60 / 28.71 ms |

HEAD reports, in table order:
`sha256:f5e800d0f081903869d7713f29631daba77373fd98cc403a6a1f852604c0e75d`,
`sha256:112398773dddf5ee423700f68dfd03c757e277abde474186ff56df9d67c9ab97`,
`sha256:9c8aae3e62072d37bab0a92b1a3c1b2ebdd54dd8b98fd719771165e2266d346d`,
`sha256:6ca66b2bb8ded1d67c6cbec83ff61597c942d1d5ab9aa8270a5f4536785eab2a`.
New reports, in table order:
`sha256:b703a4440f25199a0186a6855a03dc857d049db2046fdeed9511478861edd331`,
`sha256:77b41c3a832ea644c40af4a209bee0ee0f4db3452121722ef8ca43e608f25b8b`,
`sha256:4a7f4f6f9913c1c8f3ef6f93942aa11979aa8b08f3903e483c0e4667dc0ae8ab`,
`sha256:24567d77a583888d1575e6b988a23df65c40855398e23065f7447d13d594e891`,
`sha256:dc5df88170f4de67ebf63c55f23483434ac66e8757970a746aa844b58efacaa2`,
`sha256:46874c6d84b1e12e6c860800c2d03dffd15bfee1b6a6c24193eda56bd0a9b919`,
`sha256:d15938024eaba05851c1a86128db90574c4b1ca40027dd9bea66cf20b680de16`.

Sixteen lights with four shadowed now add 1.7 ms median and 5.0 ms p95 to
the frame, against 4.1 and 10.6 ms at HEAD; the tier still misses the
budget. Sixteen unshadowed lights add 1.3 to 1.4 ms median to
`Tiled.Opaque`. Of
the shadow saving, the single tap gives 0.66 ms median and 1.64 ms p95: in
an earlier pair of runs in the same session, the shadowed case measured
11.24 / 19.57 ms with the Poisson filter without refractive layers
(`sha256:c2c158ab7f61889a817a62b3a2a373767f04d91a67b3e2e31bb61ff5a16338f2`)
and 10.58 / 17.93 ms with the single tap
(`sha256:e3e07a7e5c988853ce73208989513c38bf744de81492ea0555db430542595ca8`).
One rectangle light keeps `Tiled.Opaque` near 15.5 ms median and 29 to
31 ms p95 in both builds: its path and registers dominate that variant.

On a temporary night copy of `tiled_bistro_baked_dynamic_capture` looking at
a hedge under a shadowed spot, the single tap changes 0.7% of pixels by more
than 8 of 255, all along shadow edges, which become harder and stepped; the
daylight capture changes by at most 6 of 255, in the clouds.

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
