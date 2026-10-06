---
status: partial
updated: 2026-10-06
authority: adr
---

# ADR-087: Graphics pipelines per GPU class

## Status

Accepted (partial). The decision is in force. Since 2026-10-06 the class
follows the backend (decisions 1 and 7): Metal runs the tiled pipeline,
including in the editor (decision 9), and Vulkan the desktop pipeline. The
tiled pipeline draws glass (decision 10) and a bounded set of dynamic lights
(decision 11), and holds the frame with adaptive quality (decision 12). On a lightmap-baked
Bistro with cooked mesh levels
([ADR-085](085-gpu-geometry-lod-and-terrain-geomorphing.md#cooked-mesh-levels))
it takes 15.5 ms p95 at native scale and 14.9 ms with adaptive quality, within
the budget; 16 dynamic lights reach 20.7 ms at native scale and, before the
levels, 17.1 to 17.5 ms at the lowest scale (see
[Adaptive quality measurement](#adaptive-quality-measurement)).
The remaining design is in
[Tiled graphics pipeline](../proposals/tiled-pipeline.md).

## Context

Before this decision the renderer had one graphics pipeline for every
supported GPU, implemented with the same semantics on Metal and Vulkan
([ADR-044](044-shader-cross-backend-contract.md)): a visibility buffer, a
compute G-buffer resolve, compute deferred lighting, four transmission
layers, screen-space effects and temporal reconstruction. It is now the
desktop pipeline ([ARCHITECTURE.md](../ARCHITECTURE.md#rendering-pipeline)).
The supported GPUs differ in kind: Apple M1 to M4 are tile-based deferred
renderers with unified memory, RDNA 2 and Ampere are immediate-mode discrete
GPUs ([ADR-083](083-supported-hardware-matrix.md)).

The owner's targets are different per class. Discrete GPUs should go as far as
the hardware allows, including ray and path tracing. Apple M-series should run
at native resolution without upscaling at 60 fps, and its pipeline should be
the base for a later mobile renderer.

That pipeline could not meet the M-series target. On the M1 Pro, its Metal
implementation, since removed, rendered Bistro with production effects at
2560×1440, render scale 1.0, portable TAA and no upscaler in 56.12 ms mean,
52.88 ms median and 71.38 ms p95 per frame, and the frame was GPU-bound
(Release, `local-windowed-gpu-single`, one process, 120 measured frames,
report digest
`sha256:1e63ae765ec38fb310c031a0a2f0abbfff50c72caae407956ee7ca9bb54b54dd`,
2026-10-05; a temporary copy of `bistro_metal_production_040`, a Metal
desktop case since removed, with `render_scale` 1.0). Compute passes took 49.5 ms and raster passes 5.8 ms of
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
   through dynamic-rendering local read. Today each backend implements one
   class (owner decision, 2026-10-06): Metal the tiled pipeline, because
   every supported Mac GPU is an M-series tile-based GPU
   ([ADR-083](083-supported-hardware-matrix.md)), and Vulkan the desktop
   pipeline, which serves the immediate-mode discrete GPUs on Windows.
2. A class that both backends implement shares rendering semantics and
   evidence states as [ADR-044](044-shader-cross-backend-contract.md)
   defines. Today each class has one backend, so its native evidence comes
   from that backend alone. Parity is not required between pipeline classes.
3. Both pipeline classes share an art-level contract: the material model and
   its inputs, the color pipeline (exposure, tonemapping, display transform and
   color grading), scene and asset data, and the render graph, upload,
   residency and animation infrastructure. Lighting, global illumination,
   shadows, anti-aliasing, screen-space effects and quality presets may differ.
4. The tiled pipeline budget on the M1 Pro is Bistro at 2560×1440, render scale
   1.0, no upscaler, 16.7 ms p95 (60 fps). Quality tiers that lower M1 cost are
   allowed.
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
   unchanged. The cloud trace needs that depth, so `Tiled.Atmosphere`
   (`pass.tiled.atmosphere`) afterwards lays the cloud layer and the discs it
   lets through over the resolved sky pixels, and aerial perspective and
   analytic height fog over every pixel, as the desktop pipeline's
   `Fog.Apply` lays them; it runs while clouds, aerial perspective or fog
   are on. A pixel takes its nearest sample's depth, so edge pixels mixing
   sky and surface samples miss the clouds and take the surface's media.
   Opaque draws shade at the laid depth with
   fragment variants that never discard, so hidden-surface removal keeps its
   fast path; alpha-tested draws, which the pre-pass skips, shade after them
   with their alpha sharpened about the material's cut-off to a transition
   about one pixel wide and turned into covered samples (alpha to coverage).
   Before the resolve, a tile kernel (`vkr_metal_tiled_resolve_tile`, one
   thread per pixel of the pass's fixed 32×16 tiles) replaces each pixel's
   samples with their average weighted by
   1 / (1 + largest channel), so edges against bright surfaces resolve as
   smoothly as dark ones. The tiled class draws no FXAA (owner decision,
   2026-10-06): its geometry and alpha-tested edges are multisampled, while
   glass, blended surfaces and shading detail inside a surface are not
   anti-aliased. The tiled graph has no `Post.Bloom.Combine`: its tonemap
   passes sample the bloom chain's first level themselves
   (`VKR_METAL_PACKET_TONEMAP_FLAG_BLOOM`) with the combine's FP16 rounding,
   so the image is unchanged and the `hdr_combined` capture channel is
   unavailable on tiled.
7. The class follows the renderer's backend
   (`vkr_graphics_pipeline_for_backend` in
   [`vkr_renderer.h`](../../renderer/src/vkr_renderer.h)): Metal runs the
   tiled class without temporal upscaling, Vulkan the desktop class. Nothing
   else selects it. A harness case records the class its backend resolves to
   on the host in its report and workload fingerprint, so no desktop
   evidence matches a tiled run, and a case that names a render mode or
   replay channel the tiled class does not draw fails on Metal
   (`vkr_graphics_pipeline_draws_render_mode`). The tiled class turns
   off temporal reconstruction, SSR, SSGI, GTAO,
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
   diffuse volume, else the global environment. The first active layer's
   alpha, the set's baked ambient visibility, multiplies the material
   occlusion that occludes environment specular; the lightmap's diffuse
   light already holds its occlusion, so it does not scale diffuse light.
   Applying it cost nothing measurable: `Tiled.Opaque` on the baked orbit
   7.95 / 12.39 and 7.94 / 12.46 ms before, 7.75 / 12.26 and 7.76 / 12.06 ms
   after
   (`sha256:43f91acefad036ef9bdbff5e3365b639f64daee0f6a5387b0ab116314d70e2a9`,
   `sha256:4acbb6970105fbab718dcf2f1e2bbba1f3bde70fc7baa05b8e0e7806947fe4cb`,
   `sha256:3d997af39190c2f9509c53699ecb461bb6158eb84bd7c8a6d4034d28caec4a91`,
   `sha256:12fddc9dd2ba7295ac64a6fdccf432e849011c4b276d4ddca3c0e0cc4cd7ce07`;
   2026-10-06, both on the set with visibility). A terrain material blends its
   four layers by the vertex colour's weights as the G-buffer resolve does
   ([ADR-084](084-agent-channel-and-level-design-toolkit.md)), reading the
   material table's terrain rows through the frame root. Blended surfaces,
   drawn after `Tiled.Atmosphere`, apply aerial perspective and fog to their
   own light only. Reflection probes give one environment per surface: the
   camera view's encode kernel assigns each draw the frame probe whose
   influence at the draw's bounding-sphere centre is largest and at least
   one half, in the visible row's `state_flags` bits 27 to 31
   (`VKR_GPU_DRAW_PROBE_SHIFT`, `vkr_metal_packet_draw_probe` in
   [`gpu_draws.metal`](../../renderer/src/shaders/metal/msl/world/gpu_draws.metal)),
   and the surface takes that probe's box-projected prefiltered cube and
   SH diffuse in place of the global environment; glass picks its probe per
   pixel by the same rule. The desktop pipeline instead blends every probe
   per pixel by influence; that blend cost the tiled pass 0.35 ms median and
   0.75 ms p95 on Bistro and was rejected. Frames whose camera frustum meets
   no probe's box and blend band take shading variants without probe code
   (`vkr_metal_packet_tiled_probe_variant`). Clearcoat, sheen, anisotropy and
   diffuse transmission are not drawn yet. The
   vertex stage is MSL, because the Slang module numbers entry point buffers
   in declaration order and the GPU-encoded commands bind the draw root at
   buffer 0.
9. The editor runs the tiled pipeline. The tiled graph carries the editor's
   Scene image passes (resolve, grid, selection mask and outline, overlay
   and composite), which read the resolved `hdr_scene_color` and
   `opaque_vbuffer_depth`. Without a visibility buffer, a pick replays the
   opaque GPU-driven draws into the picking target (`Picking.Tiled`,
   `pass.picking.tiled`); picks are rare, so frames without one pay nothing,
   and an alpha-tested surface picks whole. The pick readback also copies the
   picked pixel's resolved depth, which grid fit reads in scenes without
   collision. Graphics settings written while the class was a preference
   still load and ignore the retired `tiled_pipeline` key; on Metal,
   temporal upscaling is unavailable and dynamic resolution steps the
   spatial upscale. A project-managed editor starts from a machine-local
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
11. The tiled pipeline lights a baked scene's static lights only through its
    baked data, the lightmaps and diffuse volumes, and draws a bounded set of
    dynamic lights forward. With `static_lights_baked`, which the runtime sets
    on the tiled class for each scene with a loaded lightmap set, the
    lighting system leaves that scene's static lights out of its tables; a
    scene without one keeps them, so they draw with the dynamic lights until
    it is baked (owner decision, 2026-10-06). Then
    `vkr_lighting_system_limit_point_lights`
    ([`vkr_lighting_system.h`](../../runtime/src/renderer/systems/vkr_lighting_system.h))
    keeps the 16 dynamic point and spot lights nearest the camera by the
    distance to their range, and lets the 4 nearest shadow casters among
    them cast shadows. The forward shader evaluates them through the shared
    light grid and local-light loop (`vkr_metal_packet_punctual_layered`),
    and dynamic rectangle lights through the shared LTC path. A shadowed
    light takes four bilinear comparisons of its local shadow map half a
    texel apart, a 3x3 tent kept a texel inside its face
    (`vkr_metal_packet_local_shadow_sample<false, false, true>` in
    [`sampling.metalh`](../../renderer/src/shaders/metal/msl/shadow/sampling.metalh)),
    instead of the desktop Poisson disk, so its shadow edge is smooth over
    about a texel but has no soft penumbra (owner decisions, 2026-10-06). A
    light the shadow system marks reduced keeps one comparison. The tent
    cost `Tiled.Opaque` on the 16-light orbit about 0.1 ms median and p95
    over one comparison (9.85 / 17.66 and 9.77 / 17.44 ms before, 9.92 /
    17.63 and 9.92 / 17.74 ms after;
    `sha256:752276eac612fea43fe5c24c24d63689e5e6a03a2f25afbe55ff53d7aa6eb5c7`,
    `sha256:da583340a13366a4aff7ee6870b97f4be6334478529f1228c5f47e53b58ff15c`,
    `sha256:e2d293c0b3d7c19b7fbadc6c053208d650ba5ccd98ac2caa13cf2138d5f055cd`,
    `sha256:8df0a2f181136be51ab40c98e7d3a7ad1ca06006e8e21a5998619c152092b87b`).
    The tiled graph renders the local shadow atlas with the desktop passes
    `Shadow.Local.Clear` and `Shadow.Local`; glass casts no local shadow, so
    the sampler reads no refractive layers. Each frame shades with the
    fragment variant for its dynamic lights (`VkrMetalTiledLighting` in
    [`tiled.metal`](../../renderer/src/shaders/metal/msl/world/tiled.metal)
    and `vkr_metal_packet_tiled_lighting`): none, point and spot lights
    without shadows, with shadows, or rectangle lights as well; the editor's
    unlit, detail lighting, lighting only and wireframe modes take an
    inspection variant with every light, so lit frames pay nothing for them.
    Light code a
    frame does not use still costs `Tiled.Opaque` its registers with no light
    in range, on Bistro 0.65 ms median for the light loop, 1.2 ms for the
    former shadow filter and 1.1 ms for the rectangle-light path.
    Rectangle lights have no range, so the tiled shader skips a rectangle
    whose contribution bound falls below the cut-off deferred shading
    applies to punctual lights (`VKR_LOCAL_LIGHT_CONTRIBUTION_CUTOFF`): its
    solid angle, bounded through the distance to its nearest point, times
    its luminance and the receiver's largest diffuse and GGX specular
    response (`vkr_metal_packet_layered_rectangle_lights<true, true>` in
    [`lighting.metalh`](../../renderer/src/shaders/metal/msl/world/lighting.metalh)).
    A glossy receiver keeps distant rectangles it can reflect.
12. Adaptive quality (owner decision, 2026-10-06, after Valve's VR adaptive
    quality): with dynamic resolution on, the tiled pipeline steps its
    internal render scale ([ADR-039](039-metal-internal-render-scale.md))
    between 0.65 and native by 0.05 to hold a 16 ms GPU frame, and its
    tonemap pass upscales the scene spatially to the output. Native
    resolution stays the target whenever the frame fits. The controller
    ([`vkr_dynamic_resolution.c`](../../renderer/src/vkr_dynamic_resolution.c))
    keeps fixed-size CPU state and consumes only completed GPU submission
    times tagged with the scale they ran at; it ignores duplicate samples and
    samples from another scale. It follows the raw times: two frames over
    the target step down, two steps when both exceed it by a quarter, and
    thirty frames below 80% of it step up. When a step up exceeds the target
    and falls back, the controller keeps the measured cost ratio of the two
    scales and steps up again only when the lower scale's time multiplied by
    that ratio falls below 80% of the target, so unchanged work does not
    probe the same failure again; sustained headroom at the upper scale
    clears the ratio, and a change of the Scene output extent clears all
    learned timing but keeps the scale. A step recreates the viewport-sized
    graph images, so the cloud and HZB histories restart, and the memoryless
    multisampled targets retire until the GPU completes the frames that drew
    into them instead of stalling it. The sample's tiled settings use 0.65
    to 1 and 16 ms
    ([`vkr_sample_runtime_config.c`](../../runtime/src/vkr_sample_runtime_config.c));
    the dynamic-resolution setting turns it off. Vulkan has no dynamic
    resolution; FSR 3.1 takes a fixed scale
    ([ADR-052](052-vulkan-fsr31-upscaling.md)).

### First tiled pipeline measurement

Release, M1 Pro, 2026-10-06: the production Bistro orbit at 2560×1440 pixels,
render scale 1.0, 120 warm-up and 300 measured frames, one process per run,
`./build_release/tools/vkr_harness profile --case
tools/cases/local/tiled_bistro_native.case.json --profile
tools/profiles/local-windowed-gpu-submission-single.json` and its desktop
counterpart `tiled_bistro_native_desktop`, which ran the desktop pipeline on
Metal and was removed with it, run alternately twice:

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
of both pipelines (`tiled_bistro_capture` and the removed Metal desktop case
`tiled_bistro_capture_desktop`) match in sky and sunlit surfaces; the tiled
one lacks lamp light, glass and ambient occlusion.

### Glass evidence

Release, M1 Pro, 2026-10-06: the Bistro orbit with pass timestamps
(`tiled_bistro_native`, `local-windowed-gpu-single`, report
`sha256:88d5f93cdc606960a49341ae3d0622b7b4cefa3f9561aff1428b9054edaf26a9`)
draws 325 blended draws, all glass, and `Tiled.Blend` takes 0.40 ms median
and 0.60 ms p95. Captures of the café windows on both pipelines
(`tiled_bistro_glass` and the removed Metal desktop case
`tiled_bistro_glass_desktop`, reports
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
pipelines, the desktop one on Metal before its removal, with the same pool, pillar shadow and tint, over the baked lamp
groups on the tiled one. `test_point_light_limit_keeps_nearest` and the
time-of-day lighting test cover the limit and the static-light filter.

### Editor evidence

Release editor, M1 Pro, 2026-10-06, isolated `HOME`. On the toolkit test
level, a headless run selecting the tiled pipeline (then a graphics
preference, since retired) wrote the machine-local `graphics.json`; the next
start ran the tiled pipeline with `gfx.restart` false after the project
opened, a click picked the floor and its outline,
grid and labels drew. On Bistro, which has no collision, `camera.view top;
grid.fit` read the picked depth as 16.8432 m on the tiled pipeline and
16.8431 m on the desktop pipeline's removed Metal implementation. The CPU test `test_tiled_graph_topology`
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
static lamps at runtime; its row ran on the removed Metal implementation
(`tiled_bistro_baked_native_desktop`, since removed).

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
The rectangle cut-off of decision 11, measured the same day against the
build before it with one run each, lowers it from 15.38 / 28.83 ms
(`sha256:77ab400225bad60adebaeae243b17853c4473c01160589af5500b6106f97beac`)
to 13.79 / 26.28 ms
(`sha256:f6168aec396c8a082c6abada566cd249aa375bd21fb0134bb5ff71282753154a`);
the case without a rectangle light measured 10.90 / 18.24 and
10.82 / 18.10 ms. Night and daylight captures with the rectangle light
changed by at most 4 and 8 of 255, in 6 and 24 pixels above 2.

On a temporary night copy of `tiled_bistro_baked_dynamic_capture` looking at
a hedge under a shadowed spot, the single tap changes 0.7% of pixels by more
than 8 of 255, all along shadow edges, which become harder and stepped; the
daylight capture changes by at most 6 of 255, in the clouds.

### Anti-aliasing measurement

Release, M1 Pro, 2026-10-06, the cases and settings above. HEAD `1cb001b5`,
copied with its shader catalog, and the build with alpha to coverage, the
tone-mapped resolve and no FXAA ran alternately in one session, two runs
each. An earlier series that session ran with a virtual machine and a video
call loading the machine; its absolute times were 25 to 30% higher and are
not used.

| Case | HEAD median / p95 | New median / p95 |
|---|---|---|
| `tiled_bistro_baked_native`, `gpu.submission` | 14.79 / 19.68, 14.49 / 18.59 ms | 13.10 / 17.88, 12.87 / 16.98 ms |
| `tiled_bistro_baked_dynamic_native`, `gpu.submission` | 16.59 / 25.24, 16.34 / 23.75 ms | 15.47 / 25.20, 14.65 / 22.06 ms |
| `tiled_bistro_baked_native`, `Tiled.Opaque` | 9.08 / 13.02, 9.22 / 13.14 ms | 8.74 / 12.64, 8.93 / 12.65 ms |
| `tiled_bistro_baked_dynamic_native`, `Tiled.Opaque` | 10.99 / 18.39, 11.07 / 19.21 ms | 10.71 / 17.98, 10.66 / 17.89 ms |

Pass timings use `local-windowed-gpu-single`, the others
`local-windowed-gpu-submission-single`. Without FXAA the frame drops
`Post.DisplayLinear.Fullscreen` (0.50 ms) and `Post.Tonemap.Fullscreen`
falls from 1.30 to 0.50 ms. With the hardware resolve instead of the tile
kernel, the same build's `Tiled.Opaque` measured 8.41 / 12.23 ms in the
loaded series, which puts the tile resolve near 0.3 to 0.5 ms. Reports:
HEAD `gpu.submission`
`sha256:20485edbfd3e45289333e8e72d84faddffd1ebb573ab3d5cfb7afb8f59b4067a`,
`sha256:d5e24fb2845626d6222f26c5d183869cb4da6197f09f10e2f2362617986295c2`,
`sha256:8f16132663b02acd8157056bbedc0d88862076ae4c675dba5b0acc7aa5319ee8`,
`sha256:681f8100af0785d15afcc60e5bb38a5b69d7693e175d1827cd0604172fa3fb1b`;
new `gpu.submission`
`sha256:625368a53d303a104824cc8adf6865fa0172f550293042b0e95b0ccf842ed245`,
`sha256:5b145e6f2904f110f309e4d04538cffde58da0824d2c055afd16010a856c3894`,
`sha256:c0a6ebcbf6207656958a6f4bb4602c102eb840f1e6a8dec7962fe86ed543d875`,
`sha256:f8395307dbf4f0e4add9ba49a0bf0adba6e711b0a8a2300f94498759881e6745`;
HEAD pass timings
`sha256:46e242c8c31e274fe9bfe426392a78c37fe6b0938f7fec2a187361b2cd2d2e26`,
`sha256:9c68612e73b06399fb85899b46785cfa359f7f3c976870d320389b71ea8c8015`,
`sha256:e60c2474f9a80d1bd4c18857e0f228d0e64cc68d5b4b12b0b866398302ce7495`,
`sha256:4c0091d4625ff697960b54379debed6864368781ee3f766f2467e06e966281f3`;
new pass timings
`sha256:f53f5122f7a4a6faaa8000652fbc8e7b6f801a469c07b35de79ab354b15b003f`,
`sha256:4d124bd67bb91b6d5ee4f8c277e5c11d140f6aa2dfd3a0efed3c4574b41bdaef`,
`sha256:fad58182c3f771aee12bba3cca0fa7cc81f9fd4c7315c55bb0befeeaa2ff346b`,
`sha256:eea3ace9a8a5dfe20e4f81b2b72d75d6d17486e7ca13b5ce893e88c96ad5c503`.

In `tiled_bistro_baked_capture` and a night view of the hedge
(`sha256:6d4ad123e1e1e610f09713d0a84ebed4820378bbb5b656a5587c37d76ea0e790`,
`sha256:05f55a325c1091f23566808b11c9639921d99d2230d6fa19c4dfc80bceb66253`),
the image is sharper than with FXAA, and geometry edges, a curb and a window
grill, resolve with intermediate steps. Lit foliage shows more texel-scale
speckle than FXAA left: its holes and lighting vary inside the surface, which
neither MSAA nor alpha to coverage filters.

Skipping alpha-tested shading altogether lowers `Tiled.Opaque` from
8.50 / 12.44 to 7.61 / 10.50 ms (diagnostic, incomplete image), so foliage
costs about 0.9 ms median and 2 ms p95. Laying its covered depth in the
pre-pass as well, with the same coverage code so both passes cover the same
samples, left the image unchanged (one pixel above 8 of 255) but raised
`Tiled.Opaque` to 9.00 / 12.58 ms median / p95 against 8.50 / 12.46 ms, and
10.75 / 17.64 against 10.29 / 17.84 ms with dynamic lights, alternating two
runs each; it was not kept
(`sha256:4d0422a1272a83a3997e97541a63f78af7607fc03a70d0116a96e47e19e9c83c`,
`sha256:35c2dfd930e5b13712d5dedebdb51b727a459eb59556b860915972d4831c9195`).

Folding the bloom combine into the tonemap passes, measured the same way
(HEAD `53e5fd6e` with its own graph), removes the 0.43 ms
`Post.Bloom.Combine` while `Post.Tonemap.Fullscreen` grows from 0.51 to
0.69 ms; `tiled_bistro_baked_native` takes 12.87 / 16.81 and
12.78 / 16.88 ms `gpu.submission` before and 12.59 / 16.55 and
12.55 / 16.70 ms after
(`sha256:f5cb163abc7a9e2818f4209ba72f74ab8cb4baf848487c4e09edec1459907444`,
`sha256:537ec203e32e5464568436e89ab9c8921678ab6962a9c8e67040a434af08a89e`,
`sha256:0bc446fb946c293205ddf96ca719a69206ac2b87d91a41090cfd460d4f0c1701`,
the third `incomplete` on warm-up stability,
`sha256:8abdec9b060d58607156397dc2eb0cee378821d39e3b6b9f6f12127b074ceff4`).
The street, night and editor captures match the combine within 7 of 255 in
at most 4 pixels.

A per-frame cap on cascade re-renders was considered for the p95 frames and
not built: in one `gpu.submission` run of the new build (`sha256:0bc446fb946c293205ddf96ca719a69206ac2b87d91a41090cfd460d4f0c1701`),
220 of 300 orbit frames render no cascade (median 12.37 ms), 76 render one
(13.82 ms) and 4 render two (15.00 ms); frames without a cascade render reach
18.1 ms, so the p95 follows the view's opaque cost rather than coinciding
re-renders.

### Adaptive quality measurement

Release, M1 Pro, 2026-10-06, one build, `local-windowed-gpu-submission-single`,
the fixed-scale and adaptive cases alternating twice
(`tiled_bistro_baked_adaptive_native` and
`tiled_bistro_baked_dynamic_adaptive_native` are the base cases with
decision 12's range and target):

| Case | Median / p95 | Frames over 16.7 ms | Mean scale | Steps |
|---|---|---|---|---|
| `tiled_bistro_baked_native` | 12.68 / 16.87, 13.19 / 17.97 ms | 16, 39 of 300 | 1 | 0 |
| `tiled_bistro_baked_adaptive_native` | 11.58 / 15.80, 11.67 / 15.80 ms | 12, 13 | 0.89 | 16, 14 |
| `tiled_bistro_baked_dynamic_native` | 15.01 / 22.94, 14.93 / 23.30 ms | 114, 107 | 1 | 0 |
| `tiled_bistro_baked_dynamic_adaptive_native` | 11.07 / 17.07, 11.24 / 17.45 ms | 22, 22 | 0.73 | 22, 20 |

Reports, in table order:
`sha256:f4d81eab110b82edc5e26e609316520cad0b0d03145c0445803f4fd09bc7457e`,
`sha256:aeb29c087409d8241cc1afcb2cef37ff992821affbaa130b24aef9838f4d6fbf`
(`incomplete` on warm-up stability),
`sha256:ea3007850f51db45c71cd5d8f789756e22911b1004b722903cfbd1a56d29c4ec`,
`sha256:88c122a9e2f6f31f2d06ba1d4ee58dd1881f5c941897d6d662a3242697eaed90`,
`sha256:72942fdb8d10ee8fb1660571e82361c8bbbdc7d19864e5ef680da655e89662b0`,
`sha256:84c90d05d6d5615121db41ba2b8a7b18d39621ff07bf13b8bf4ee347bfcfcfce`,
`sha256:bb2baa4bc314efcf6da41e5cc6a519c925ac58ed668002a00cc7733053044aeb`,
`sha256:775e44c5461e1413740d7c6e85484891f0e82121dff3979b6e7c792f8e14b692`
(`incomplete` on warm-up stability).

The orbit's heavy stretches last 35 to 40 frames. With the 16 dynamic lights
the scale holds 0.65 to 0.75 through them, and the heaviest views still exceed
16.7 ms at the floor. Frames that change scale took up to 19 to 22 ms of wall
time against a 17.0 to 17.6 ms p95. The filtered controller that MetalFX
used, removed with it, stepped once per 33 frames and left the p95 at 16.6
and 20.6 ms. A 0.75 capture
of `tiled_bistro_baked_capture` is visibly softer at text and thin edges than
native. Metal API validation passed a run of the dynamic-light case that
stepped six times (4 ms target).

### Unbaked lights, terrain, inspection and atmosphere

Release, M1 Pro, 2026-10-06: the build before these changes
(`VKR_SHADER_CATALOG` copy) alternated with the build after them, two runs
each, both forced to the tiled class; `gpu.submission` median / p95 from
`local-windowed-gpu-submission-single`:

| Case | Before | After |
|---|---|---|
| `tiled_bistro_baked_native` | 11.33 / 15.46, 11.35 / 15.40 ms | 11.55 / 15.76, 11.54 / 15.79 ms |
| `tiled_bistro_baked_dynamic_native` | 13.23 / 20.64, 13.25 / 20.63 ms | 13.44 / 20.87, 13.50 / 20.88 ms |
| `tiled_bistro_native` (no lightmap set) | 10.24 / 13.87, 10.25 / 14.01 ms | 16.35 / 24.08, 18.12 / 26.95 ms |

Unbaked Bistro now draws its glTF lamps as dynamic lights, 16 of them with 4
shadowed, instead of none; adaptive quality absorbs that until the scene is
baked. With pass timestamps (`local-windowed-gpu-single`), `Tiled.Opaque` on
the baked orbit went from 7.43 to 7.57 ms median and 11.50 to 11.82 ms p95 to
7.54 to 7.67 and 11.66 to 11.94 ms, and the atmosphere draw took 0.38 ms
against the cloud draw's 0.32 ms. In earlier builds of this change, applying
aerial perspective in the forward shading cost `Tiled.Opaque` 0.5 ms median
and 1.2 ms p95 even with the lookup read by reference, which is why it moved
to the full-screen draw; the terrain branch costs about 0.07 ms median and
0.24 ms p95 on a frame without terrain. Reports, before then after: baked
`sha256:9ca665c6f4b300b6e7d52a4b163b14845a79b1824ac9df147e1bd2c84ba467ef`,
`sha256:97c82fb42077e43053540a6131a997f2f291bc3089a22b591df285d430bd670c`,
`sha256:c8a1b015383043fc0aa76ea3b25f5d0ed5433c14861ad0d0409b4b343c854742`,
`sha256:18432cbbf61abd424eb62118c2ac9ec9840b380471de901154cc4b50ae374c52`;
dynamic
`sha256:a34c57a772c729d3135c9198841306e7deeb392755fdea97eb62121f4507db60`,
`sha256:68a586c3f435a3108587468b3c37300d85be088365b6e4854545fa53bb9538c7`,
`sha256:67e425fbb25ab2d2f33fac739d9df3750d2d6e9c3f6d6ab7b21118284d4c927d`,
`sha256:c1f306cb31965da4f87a2c334972b8a8bd52e72c612b1d4d6ab05f06f871550b`;
unbaked
`sha256:828fd7ee343c4c1ce708130cf8f879b126f6636d4bf669122b574f23a993b1cf`,
`sha256:e35b2226629593cd9607c51ddbed699264cbe0adbc7cb3bb236e4b654270c473`,
`sha256:62fc47a50dde9ffa2db38940c0846b49b93f1545d2ec5482a807ac536bbede95`,
`sha256:785e1cc481c9183709ef459c1024686fa2ff7fa6ed3cf5d4dd630c92ff03607e`.

By inspection against the desktop pipeline's removed Metal implementation:
a 128 m terrain painted with its four layers in the headless editor shows
the same layer regions; harness captures of unlit, detail lighting, lighting
only and wireframe match their desktop counterparts (unlit within about 1 of
255), and a Bistro capture with height fog shows the same haze and sky (sky
means within 0.2 of 255).

### Reflection probe measurement

Release, M1 Pro, 2026-10-06, the build before probes (`VKR_SHADER_CATALOG`
copy at `982b3d77`) alternating with the probe build, two runs each, on
`tiled_bistro_baked_native`, whose one indoor probe (specular intensity 0,
diffuse 1) is in view for much of the orbit. With pass timestamps
(`local-windowed-gpu-single`), `Tiled.Opaque` went from 7.67 / 12.24 and
7.72 / 12.24 ms median / p95 to 8.03 / 12.44 and 7.85 / 12.77 ms
(`sha256:7f53db6b4f0bfc7beed82f65c3ec9b72f82dc39d119ecbe2092dee576125698c`,
`sha256:0a253bb36f5af95e1cc66bfa1497c3e90967c1bed45f873f032324c1069cbf67`,
`sha256:a53b797c6733ca3514ea7016db5dbc0a6392fde470addb4d6ce730b6e7b1c233`,
`sha256:95d32ab172de25be3b5d3a0d52c8fbbf3b3f7c12e1111bbf83acbe5aca4e2d69`);
`gpu.submission` (`local-windowed-gpu-submission-single`) went from
12.24 / 17.45 and 12.24 / 17.43 ms to 12.21 / 16.99 and 12.23 / 17.39 ms
(`sha256:f9825c717a19cbd810e27a69c94c67a9e974ec467dfcfbc93a77c0cb04c6abbd`,
`sha256:a823875b0a2617a747807f79ed58f8ba57e78e23e528043626571b32bdff19d0`,
`sha256:7a8581cb5cd5831a663a2c4c1fc7b46fd19d3b5b3ae9610f982755b6aadce17c`,
`sha256:c10e846078a7540948bfa5da5e0e4e3174f3b6f87e3cb1697d5b5ce6619334ca`).
Both builds ran about 1.7 ms slower at p95 than the same case in the earlier
measurements of this ADR, with a browser drawing on the same GPU; the runs
alternate, so the comparison holds. A per-pixel blend of the probe and the
global environment, measured the same way, raised `gpu.submission` p95 from
16.5 to 18.2 and 18.5 ms
(`sha256:5d0309ed5906bdd54a55ff08a445b142bd43faf071d9b67f874cbf35eb29412b`,
`sha256:1d5ff4c2897063fc890206b2ddb1bde9d04a37653ae3c1c9e4e9f9263fe086d3`,
`sha256:1c576286e3c0145321510010fa2a350b891938eccc9718efe5eb6abfa0dc4730`,
`sha256:ec2ad5c85e8de23f99935bbf3504111aabefd2728f7a5b51ae5b19e744835bc2`).
A temporary interior view with the probe's specular intensity raised to one
reflects the room's cubemap where the build before reflected the sky, and
the baked street capture differs in at most 5 pixels by more than 2 of 255.
A Metal API validation run of the interior view passed.

## Consequences

- Lighting, shadow, AA and screen-space work is implemented and validated once
  per pipeline class. Harness cases, baselines and ADR-044 evidence states are
  kept per class. Cases that need the desktop pipeline name the Vulkan
  backend, so their native evidence needs a Windows machine; the Metal
  desktop and MetalFX cases and the Metal desktop baselines were removed.
- `AGENTS.md` applies the shared rules per class: the classes share this
  art-level contract and the shared shader kernels, and with one backend per
  class a change is validated natively on its class's backend, a shared
  kernel on every backend that consumes it.
- Ray and path tracing can be added to the desktop pipeline without a
  tile-based equivalent.
- The tiled pipeline needs more baked data, which `vkr_bakery` owns
  ([ADR-077](077-asset-build-system.md)): lightmap sets
  ([ADR-088](088-baked-lightmap-sets.md)) and possibly denser probes.

## Alternatives considered

- **One pipeline with quality presets.** Rejected: the measured M1 Pro cost is
  in techniques that presets can only scale, not replace.
- **Pipelines defined by API.** Rejected as the design: a later Vulkan mobile
  renderer would need a third pipeline or a port of the Metal one. The class
  follows the backend today only because each supported GPU family has one
  API (owner decision, 2026-10-06).
- **Fixed upscaling on M-series.** Rejected by the owner: native resolution
  is the target. Decision 12 lowers it only while frames miss the budget
  (revised by the owner, 2026-10-06).

## Revisit when

- The tiled-pipeline prototype cannot reach the budget with the planned
  techniques.
- Apple9 dynamic register allocation changes the measured cost structure on M3
  or M4 ([ADR-083](083-supported-hardware-matrix.md)).
- A mobile target is scheduled and needs its own budget, or a backend must
  run a second class (a tile-based Vulkan GPU, or an immediate-mode GPU on
  Metal).
