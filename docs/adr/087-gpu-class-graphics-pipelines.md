---
status: partial
updated: 2026-10-30
authority: adr
---

# ADR-087: Graphics pipelines per GPU class

## Status

Accepted (partial). The decision is in force. Since 2026-10-06 the class
follows the backend (decisions 1 and 7): Metal runs the tiled pipeline,
including in the editor (decision 9), and Vulkan the desktop pipeline. The
tiled pipeline draws glass (decision 10) and a bounded set of dynamic lights
(decision 11), and holds the frame with adaptive quality (decision 12). Since
2026-10-16 the MetalFX temporal scaler rebuilds its Scene from 0.6 of each
axis by default ([ADR-121](121-tiled-temporal-upscaling.md)). On a lightmap-baked
Bistro with cooked mesh levels
([ADR-085](085-gpu-geometry-lod-and-terrain-geomorphing.md#cooked-mesh-levels))
it takes 15.5 ms p95 at native scale and 14.9 ms with adaptive quality, within
the budget; 16 dynamic lights reach 20.7 ms at native scale and, before the
levels, 17.1 to 17.5 ms at the lowest scale (see
[Adaptive quality measurement](#adaptive-quality-measurement)).
The remaining design is in
[Tiled graphics pipeline](../proposals/tiled-pipeline.md). The desktop
pipeline meets its budget (decision 4): with Bistro's static lamps baked, the
baseline accepted on 2026-10-22 takes 13.98 ms p95 on the orbit and 13.94 ms
on the street walk
([Desktop pipeline baseline](#desktop-pipeline-baseline)). Its 120 Hz line,
High at 1920×1080, is met on the GPU and missed by `frame.wall` by up to
0.35 ms ([Desktop 120 Hz line](#desktop-120-hz-line)).

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
4. The tiled pipeline budget on the M1 Pro is Bistro at 2560×1440, 16.7 ms
   p95 (60 fps). Until 2026-10-16 it was measured at render scale 1.0 with no
   upscaler; since then it is measured with the default MetalFX upscaling
   ([ADR-121](121-tiled-temporal-upscaling.md)). Quality tiers that lower M1
   cost are allowed.
   The desktop pipeline budget (owner decision, 2026-10-20) is Bistro at
   2560×1440 physical pixels on the RX 6700 XT, the Vulkan reference device
   of [ADR-083](083-supported-hardware-matrix.md) and the only one with
   native evidence, with the High preset, the settings' default:
   16.7 ms p95 `gpu.submission` (60 fps), and `frame.wall` p95 under the
   same 16.7 ms as the CPU check. The Scene renders at unit scale with FSR
   3.1 native antialiasing ([ADR-052](052-vulkan-fsr31-upscaling.md)).
   Since 2026-10-21 Bistro's static lamps are baked for the budget
   ([ADR-104](104-desktop-baked-lamps.md)), by the owner's decision after
   measurement: runtime lamps cost 4.0 and 7.6 ms at the median in the two
   cases, and baking them softens their shadows. The owner kept every High
   gate and FSR over the cheaper SMAA T2x on the same day.
   Dynamic resolution comes last, since a changing resolution blurs the
   image in motion. Epic has no budget. Each budget line has a p95 target,
   and the frame's margin below the budget is a reserve that features claim
   by the owner's decision. The 120 Hz line is High at 1920×1080: 8.3 ms
   p95 `gpu.submission` and `frame.wall`. The cases, profiles, line targets
   and the measured levers are in
   [Desktop budget measurement](#desktop-budget-measurement) and the
   sections after it. The baseline accepted on 2026-10-22 after the deferred
   lighting kernel change meets the budget by 2.72 ms (orbit) and 2.76 ms
   (street walk) at p95; the first baseline, accepted on 2026-10-20 with
   runtime lamps, missed it by 7.1 and 10.0 ms
   ([Desktop pipeline baseline](#desktop-pipeline-baseline)).
5. The tiled pipeline shades opaque surfaces forward in one multisampled
   render pass (owner decision 2026-10-05). On Bistro, with a depth pre-pass
   in both, it costs the same as a G-buffer kept in tile memory without
   multisampling and 1.7 to 3.4 ms less with four samples
   ([measurements](../proposals/tiled-pipeline.md#first-prototype-measurements)).
   Since 2026-10-07 it has no depth pre-pass: the opaque variants never
   discard, so hidden-surface removal already shades each sample's nearest
   opaque surface once, and the pre-pass only drew every opaque triangle a
   second time ([measurement](#depth-pre-pass-measurement)).

6. The tiled pipeline has its own render graph,
   [`tiled.rendergraph.json`](../../assets/render_graphs/tiled.rendergraph.json),
   and keeps the desktop graph's culling, cascades, sky, cloud and post
   passes. One graph pass, `Tiled.Opaque` (`pass.tiled.opaque`), owns
   forward shading and the clear sky in one render pass whose
   colour and depth are memoryless four-sample targets the Metal backend
   owns; they resolve on chip into the graph's `hdr_scene_color` and
   `opaque_vbuffer_depth`, depth to the nearest sample. The graph compiler is
   unchanged. The cloud trace needs that depth, so `Tiled.Atmosphere`
   (`pass.tiled.atmosphere`) afterwards lays the cloud layer and the discs it
   lets through over the resolved sky pixels, and aerial perspective and
   analytic height fog over every pixel, as the desktop pipeline's
   `Fog.Apply` lays them; it runs while clouds, aerial perspective or fog
   are on. A pixel takes its nearest sample's depth. The sky writes alpha
   zero and surfaces one, so the resolve leaves in alpha the share of a
   pixel's resolve weight that surfaces hold; `Tiled.Atmosphere` reads it
   through programmable blending, splits a pixel mixing sky and surface
   samples into its clear sky, which it recomputes, and the surfaces' part,
   and lays the clouds and sky fog over the first and the surface's media
   over the second (see [Sky edge measurement](#sky-edge-measurement)). It
   writes alpha one, and the tonemap presents alpha one.
   The same draw corrects the sun's light. Sun contact shadows (the Contact
   shadows setting, on the High and Epic presets of this pipeline; the
   presets differ by pipeline, `vkr_graphics_settings_apply_preset`) and
   distance field sun shadows (the Distance field shadows setting, on the
   Epic preset; the shadow quality proposal's T5) need it. In those frames (`VkrPreparedFrame.tiled_sun_contact`,
   `tiled_sun_sdf`) the opaque pass takes shading variants, selected by a
   function constant, that also write the sun's light through the clouds
   without the cascades, U, and the cascades' visibility v_c to a second
   target, `tiled_sun_light` (RGBA16F; carried through shading as one packed
   register, `vkr_metal_tiled_pack_sun`). `Tiled.Atmosphere`, which then runs
   even without media, adds `U × (v − v_c)` before the media, where v is the
   cascades' visibility or, from where the near cascades fade into the far
   level, the darker of it and the SDF trace's (`Tiled.SunShadow`), times
   the contact march toward the sun over the resolved depth. The sun's term
   is linear in its visibility, so the correction is exact: with the march's
   visibility forced to one the frame matches a frame without it bit for bit
   (Bistro street and plaza). The variants are created in the background the
   first time a frame asks for them; frames go without the correction until
   then. See [Sun contact shadow measurement](#sun-contact-shadow-measurement).
   The same draw adds the class's sun shafts, its realization of the
   scene's volumetric fog component: `Tiled.SunShafts` blurs the resolved
   depth's sky toward the sun at half resolution, and the draw scatters the
   sun's light by the component's density, colour and anisotropy over every
   pixel while the sun is in or near the view
   ([ADR-132](132-tiled-sun-shafts.md)).
   The SDF trace replaces the far level for the casters it covers (owner
   decisions 2026-10-16 and 2026-10-17, `vkr_metal_packet_sdf.inc`): every
   opaque rigid sun caster from 0.25 to 120 m whose submesh's distance volume
   is ready. Cooked meshes carry their volumes
   ([ADR-030](030-offline-mesh-optimization-and-cooking.md)), which mesh
   creation uploads; the GPU generates the rest, such as editor-built
   geometry, from resident triangles, up to 32 a frame. A field table of
   every live mesh's volumes, copied each frame,
   lets the far level's culling views skip those casters and lets
   `Tiled.SunShadow` list them in a light-space grid over every receiver the
   shadows reach (256² cells, built on the GPU each frame) before it traces.
   Terrain, foliage, cutout and skinned casters keep the far level's
   shadows. The far level is rebuilt over many frames and kept long after, so
   it may leave out only casters the trace covers: a volume stays ready while
   its mesh lives, and the grid spans the far level's reach. Once a frame
   generates no new volume after frames that did, the field's revision grows
   (`vkr_renderer_sun_field_revision`); a far layer drawn against an older
   revision is stale and rebuilds in slices
   (`vkr_shadow_system_set_field_revision`), while the kept layer, which
   still holds those casters, only doubles their shadows. See
   [SDF sun shadow measurement](#sdf-sun-shadow-measurement).
   Opaque draws shade first and lay their own depth, with
   fragment variants that never discard, so hidden-surface removal keeps its
   fast path; alpha-tested draws shade after them
   with their alpha sharpened about the material's cut-off to a transition
   about one pixel wide and turned into covered samples (alpha to coverage).
   An alpha-tested fragment reads its base colour first and discards when it
   covers no sample, before its other textures
   ([measurement](#base-pass-measurement)). Before the resolve, a tile kernel (`vkr_metal_tiled_resolve_tile`, one
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
   tiled class, with MetalFX temporal upscaling
   ([ADR-121](121-tiled-temporal-upscaling.md)), Vulkan the desktop class. Nothing
   else selects it. A harness case records the class its backend resolves to
   on the host in its report and workload fingerprint, so no desktop
   evidence matches a tiled run, and a case that names a render mode or
   replay channel the tiled class does not draw fails on Metal
   (`vkr_graphics_pipeline_draws_render_mode`). The tiled class turns
   off the desktop temporal reconstruction (SMAA T2x, FSR 3.1), GTAO,
   surface diffusion, depth of field, motion blur, froxel fog (its
   volumetric fog component draws screen-space sun shafts instead,
   [ADR-132](132-tiled-sun-shafts.md)), the local
   shadow mask, SDSM, the transmission passes, planar reflections and the
   diffuse volume's dynamic layer
   ([`vkr_renderer.c`](../../renderer/src/vkr_renderer.c),
   [`vkr_render_graph_frame.c`](../../renderer/src/vkr_render_graph_frame.c),
   [ADR-117](117-desktop-planar-reflections.md),
   [ADR-119](119-dynamic-diffuse-volume-layer.md)).
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
   2026-10-06, both on the set with visibility).
   The sun's cascades filter with a fixed grid of hardware bilinear
   compares weighted by a tent, 4x4 taps on High and 3x3 on Balanced, with
   an eight-tap uniform-region exit (`vkr_metal_shadow_grid_kernel` in
   [`sampling.metalh`](../../renderer/src/shaders/metal/msl/shadow/sampling.metalh)).
   The desktop pipeline's rotated Poisson kernel left a grain fixed to the
   screen on this pipeline, which then had no temporal resolve to average it,
   and the penumbrae slid under it whenever the camera moved (user report,
   2026-10-15). Taps stay within two texels of each other, where adjacent
   bilinear footprints meet, so the widest penumbra is three texels; the
   bounded penumbra estimate (ADR-041) runs on the three near cascades with
   an unrotated blocker search. A receiver its cascade misses takes the next
   cascade, and the cross-fade ignores a next map that misses it. On the
   Level Design Test at 1146x595 (Release, M1 Pro, 2026-10-16, headless
   editor, High, two interleaved runs each, the receiver shader alone
   swapped) `Tiled.Opaque` took 11.6 / 9.9 / 10.1 ms median in the track,
   deck and valley views against 12.2 / 10.3 / 11.0 ms with the Poisson
   kernel; that is local evidence, not an authoritative profile. Local and
   moving-caster shadows offset along the geometric normal, as the cascades
   do. The near cascades split 80 m and the last is a far level to 1 km
   (`vkr_shadow_config_apply_tiled`, ADR-041), so ridges, buildings and
   trees keep their shadows across a valley. Its static layer cost
   2.8–3.0 ms drawn at once on the Level Design Test, about every 130 m of
   camera motion; it is rebuilt one tile of an 8x8 grid a frame instead,
   0.36 ms per tile on average and 1.11 ms for the most expensive in a
   14 m/s glide along the viaduct (headless editor, 1146x595, High,
   `VKR_RG_GPU_TIMING`, 2026-10-16), and a rebuilt layer shades like one
   drawn whole. The proactive refresh spreads the near cascades' redraws
   one per frame.
   A surface with a lightmap or the volume reflects the global environment
   only in the share its baked sky light supports and its surroundings'
   baked light in the rest, so rooms and tunnels do not reflect the sky
   ([ADR-054](054-baked-diffuse-volumes.md)); that cost `Tiled.Opaque` about
   0.3 ms on the baked orbit (2026-10-11). A surface that took a probe
   skips that blend, since the capture already holds its room. A terrain material blends its
   four layers by the vertex colour's weights as the G-buffer resolve does
   ([ADR-084](084-agent-channel-and-level-design-toolkit.md)), reading the
   material table's terrain rows through the frame root. Projected decals
   then cover the base colour of opaque, alpha-tested and terrain surfaces
   ([ADR-092](092-projected-decals.md)). Blended surfaces,
   drawn after `Tiled.Atmosphere`, apply aerial perspective and fog to their
   own light only. Reflection probes give one environment per surface: the
   camera view's encode kernel assigns each draw the first frame probe, in
   the host's smaller-box-first order
   ([ADR-019](019-bounded-forward-spatial-lighting.md)), whose influence
   reaches one half over the draw's whole bounding sphere, in the visible
   row's `state_flags` bits 27 to 31
   (`VKR_GPU_DRAW_PROBE_SHIFT`, `vkr_metal_packet_draw_probe` in
   [`gpu_draws.metal`](../../renderer/src/shaders/metal/msl/world/gpu_draws.metal)),
   and the surface takes that probe's box-projected prefiltered cube and
   SH diffuse, with the probe's own gains, in place of the global
   environment. Influence reaches one half inside the probe's box shrunk by
   half its blend band at the side and top faces; a draw whose sphere that
   surface cuts, for an earlier probe, takes `VKR_GPU_DRAW_PROBE_PER_PIXEL`
   and picks per pixel, as glass always does
   (`vkr_metal_tiled_pixel_probe` in
   [`tiled.metal`](../../renderer/src/shaders/metal/msl/world/tiled.metal)),
   so a merged mesh that spans a room and the street takes the room's probe
   only inside the room.
   The first probe past one half is the one the desktop pipeline's priority
   blend gives most of the point. A probe bound to a planar reflector
   (`owner_object_id`, [ADR-117](117-desktop-planar-reflections.md)) serves
   that reflector's draws ahead of this rule and no other draw. Influence and lookup direction come from
   the shared
   [`reflection_probe_kernel.slangh`](../../renderer/src/shaders/shared/reflection_probe_kernel.slangh).
   The desktop pipeline instead blends the probes per pixel; that blend
   cost the tiled pass 0.35 ms median and 0.75 ms p95 on Bistro and was
   rejected. Frames whose camera frustum meets no sphere around a probe's
   box take shading variants without probe code
   (`vkr_metal_packet_tiled_probe_variant`). The 2026-10-12 probe changes
   on this pipeline (first-in-order choice, dominant-direction lookup, own
   gains, no baked sky scaling under a probe) were neither compiled nor run
   on Metal. Clearcoat, sheen, anisotropy and
   diffuse transmission are not drawn yet. The BRDF material extensions
   ([ADR-113](113-brdf-material-extensions.md)) are drawn: F82 tint, the
   dual specular lobe, EON diffuse and the smooth terminator. The
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
   still load and ignore the retired `tiled_pipeline`,
   `temporal_upscaling`, `screen_space_reflections` and `screen_space_gi`
   keys; on Metal, dynamic resolution steps the spatial upscale. A
   project-managed editor starts from a machine-local
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
    composes as the transmission passes do (`vkr_transmission_compose`).
    Thin glass takes the light behind it through dual-source blending: the
    fragment's second output is the factor the destination keeps per
    channel, so tinted and stacked panes compose in draw order. Glass reads
    its transmission and thickness textures through the frame root's
    transmission rows. A volume, glass with a thickness, absorbs the light
    behind it as the desktop pipeline does, `pow(attenuation colour, path
    length / attenuation distance)` along the refracted path through its
    object-space thickness (`vkr_transmission_exit_point`), and refracts it;
    rough glass blurs it. On frames whose blend list holds a volume or rough
    glass (`refractive_draw_count` in the world payload,
    `tiled_refraction_pending` in the graph),
    `Tiled.Refraction.Base` copies the image after the atmosphere draw into
    the half-resolution `tiled_refraction_source` with a 2×2 box, and
    `Tiled.Refraction.Downsample.${i}` (`pass.transmission.downsample`)
    reduces it to six levels with a four-tap box. A volume samples the copy
    at its projected exit point and rough thin glass at its own position,
    at the level the roughness picks (`vkr_transmission_rough_lod`), and
    adds that light itself, so the blend keeps none of the destination. The
    copy holds no blended surface, so such glass hides the panes behind it,
    and as in any screen-space refraction a volume's offset can pick up
    nearer surfaces. Rough glass is a material whose `rough` flag
    (`VkrMaterial`, published as `VKR_METAL_PACKET_MATERIAL_ROUGH`) is set:
    its `roughness_max`, or without one an untextured material's roughness
    factor, exceeds zero. The importer folds the roughness factor into the
    roughness texture and records it as `roughness_max`
    ([ADR-012](012-texture-compression-pipeline.md)); a textured material
    without the key counts as smooth, because its texture may hold no
    roughness at all, as on Bistro's normal-mapped panes, which carry a
    factor of one over a zero-roughness texture. Bistro's 18 glass materials
    have no thickness and no rough flag, so no Bistro frame takes the copy
    (see [Refraction measurement](#refraction-measurement)). The
    blend pass on the baked orbit took 0.47 / 0.85 and 0.48 / 0.78 ms
    median / p95 before volume absorption and 0.51 / 1.11 and 0.49 / 0.83 ms
    after
    (`sha256:043adce36b03e4b35ee87fe64b4f69c2ce7b48bf8266bf2381fcfc22bcaa0c24`,
    `sha256:d98e4027a1b5d7336414344fc5cdcefefe6f4558f561517d9dca2e809b27a072`,
    `sha256:f2919e430321daee721233ced99a5fdc3fb07baf5fc148c38d38696a0e623d36`,
    `sha256:fe4c9f0377318e0fec628d3af78b1e7090b720d9d604be7b3184f7dc50dda3d4`).
    With Bistro's glass temporarily given a 2 cm world thickness and a
    green attenuation colour at 10 cm, the restaurant interior seen through
    its windows turns cooler and less saturated. The pass runs only on
    frames with blended draws or world text, because its load and store of
    the resolved colour and depth cost about 0.4 ms at 2560×1440.
11. The tiled pipeline lights a baked scene's static lights only through its
    baked data, the lightmaps and diffuse volumes, and draws a bounded set of
    dynamic lights forward. With `static_lights_baked`, which the runtime sets
    on the tiled class for each scene with a loaded lightmap set, the
    lighting system leaves that scene's static lights out of its tables; a
    scene without one keeps them, so they draw with the dynamic lights until
    it is baked (owner decision, 2026-10-06). A set may hold each lamp
    group's direct light on denser lamp-direct pages, which the forward
    shader samples bicubically, and moving casters shadow at most two
    selected baked lamps in the `CASTERS` and `CASTERS_ALL` variants
    ([ADR-108](108-baked-lamp-direct-pages.md)). Then
    `vkr_lighting_system_limit_point_lights`
    ([`vkr_lighting_system.h`](../../runtime/src/renderer/systems/vkr_lighting_system.h))
    selects the 64 dynamic point and spot lights nearest the camera by the
    distance to their range (16 before 2026-10-28), ties by the distance to their centre, and lets
    the 4 nearest shadow casters among them cast shadows. A light selected
    or shadowed the frame before ranks as if the camera were 1.15 times
    closer, so membership does not flip while distances cross. A light
    entering or leaving the set fades its intensity over 0.25 s, the local
    shadow fade-in time: a leaving light keeps its slot until it has faded
    and loses its shadow, which the local shadow cache fades out over the
    same time (ADR-019), and an entering light takes the freed slot, so at
    most 64 draw. The first frame and a camera cut snap. Before 2026-10-08
    ties fell to table order and the set and its shadows were recomputed
    every frame, so in a brush level of about 125 unbaked lamps whose ranges
    held the camera, lights and shadows popped as the camera moved (user
    report). Since 2026-10-28 the GPU finds each pixel's lights in place of
    the CPU light grid: a tile kernel, `vkr_metal_tiled_light_tile` in
    [`tiled.metal`](../../renderer/src/shaders/metal/msl/world/tiled.metal),
    runs at the start of `Tiled.Opaque` (early and late) and `Tiled.Blend`
    and lists the lights whose sphere reaches each 32x16 tile's four side
    planes into threadgroup memory (`VkrMetalTiledLightTile`, at most 512
    entries), one SIMD-group compacting the table by ballot. The class
    keeps no depth prepass (decision 5; owner decision 2026-10-28), so the
    tiles have no depth bounds: the host sorts the rows by the near view
    depth of their sphere each frame and writes 1024 linear depth bins over
    them (`vkr_metal_packet_write_light_bins`), each the lowest and highest
    rank that reaches it, and a fragment walks only its SIMD-group's depth
    window of its tile's list, then tests each light's range
    (`vkr_metal_packet_punctual_layered`); unbounded lights are always
    walked. Dynamic rectangle lights take the shared LTC path. The tiled
    class pays for each light that reaches a pixel, not for the table: on
    the clustered Bistro street (392 lights, 1280x720, M1 Pro,
    `bistro_clustered_tiled_timing`, `local-windowed-gpu`, not
    authoritative) `Tiled.Opaque` p50 took 20.66 ms with 16 lights through
    the grid, and 21.77, 24.13, 27.96 and 37.44 ms with 16, 32, 64 and all
    lights through the tiles; on the 920-light stress street
    (`bistro_clustered_stress_tiled_timing`) 19.80 ms before and 31.83 ms
    at 64. With the kernel running and no light walked it took 9.47 ms, so
    the light loop, not the binning, holds the cost; owner decision
    2026-10-28 set 64. A shadowed
    light takes four bilinear comparisons of its local shadow map half a
    texel apart, a 3x3 tent kept a texel inside its face
    (`vkr_metal_packet_local_shadow_sample<false, false, true>` in
    [`sampling.metalh`](../../renderer/src/shaders/metal/msl/shadow/sampling.metalh)),
    instead of the desktop Poisson disk, so its shadow edge is smooth over
    about a texel but has no soft penumbra (owner decisions, 2026-10-06).
    The tiled pipeline sets `local_shadow_full_filter_all`, so every
    shadowed light takes the tent; before 2026-10-08 only the two most
    important did and the others kept one comparison, whose stepped edges
    showed on floors (user report). The tent on those two lights
    cost `Tiled.Opaque` on the 16-light orbit about 0.1 ms median and p95
    over one comparison (9.85 / 17.66 and 9.77 / 17.44 ms before, 9.92 /
    17.63 and 9.92 / 17.74 ms after;
    `sha256:752276eac612fea43fe5c24c24d63689e5e6a03a2f25afbe55ff53d7aa6eb5c7`,
    `sha256:da583340a13366a4aff7ee6870b97f4be6334478529f1228c5f47e53b58ff15c`,
    `sha256:e2d293c0b3d7c19b7fbadc6c053208d650ba5ccd98ac2caa13cf2138d5f055cd`,
    `sha256:8df0a2f181136be51ab40c98e7d3a7ad1ca06006e8e21a5998619c152092b87b`).
    The tiled graph renders the local shadow atlas with the desktop passes
    `Shadow.Local.Clear` and `Shadow.Local`; glass casts no local shadow, so
    the sampler reads no refractive layers, and no sun shadow on either
    pipeline (ADR-041). Each frame shades with the
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
    by 0.05 between a floor and the render scale setting to hold its GPU
    frame budget. MetalFX upscales the scene to the output, keeping its
    history across steps ([ADR-121](121-tiled-temporal-upscaling.md)), or,
    with the spatial upscaler, the tonemap pass. The render scale setting
    stays the target whenever the frame fits. The controller
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
    graph images, so the HZB history restarts; the cloud history is sized
    from the Scene output and kept (ADR-074). A step marks the frame with its
    own temporal reset reason, which every history but the clouds' and
    MetalFX's treats as a reset. The memoryless
    multisampled targets retire until the GPU completes the frames that drew
    into them instead of stalling it. The sample's tiled settings use a
    floor of 0.5 under MetalFX and 0.65 under the spatial upscale, and a
    default render scale of 0.6
    ([`vkr_sample_runtime_config.c`](../../runtime/src/vkr_sample_runtime_config.c)),
    and the dynamic-resolution setting turns it off. The budget is 90% of the
    frame time of the Target frame rate setting
    (`dynamic_resolution_target_fps`, 30 to 240, default 60; Cmd
    `gfx.target_fps`), so a frame keeps headroom inside its interval as VR
    adaptive-quality controllers do; a change retunes the controller
    between frames without moving the scale
    (`vkr_renderer_set_dynamic_resolution_budget`). Owner decision,
    2026-10-15: the target is a manual setting, not the display's refresh
    rate. A budget that followed the 120 Hz refresh (7.5 ms) held the Level
    Design Test at the 0.65 floor everywhere in the editor's 2061×1062 Scene
    panel on an M1 Pro (Release); the earlier fixed 16 ms budget let 10–18 ms
    frames alternate between one and two refreshes. Vulkan has no dynamic
    resolution and renders the Scene at render scale 1; the FSR 3.1
    upscaler was removed on 2026-10-12
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

### Desktop pipeline baseline

The owner accepted this measurement of decision 4's desktop budget on
2026-10-22. Release, commit `a83cc070` in a clean worktree (`dirty: false`),
RX 6700 XT with driver 26.10.1, Windows 10, Ryzen 5 2600; Bistro with its
static lamps baked ([ADR-104](104-desktop-baked-lamps.md)) and the High preset
at 2560×1440 physical pixels in a hidden window with immediate presentation.
Each report has five children, stable warmup and an exclusive GPU lane, and
is authoritative with no authority reason, GPU error or memory pressure.
Before each run the host's GPU was idle (2% or less of any engine), and no
other process used more than 1% of an engine during any run. Every frame has
0 runtime point lights (`lighting.point.selected`). The cases load
`bistro_desktop_lamps_local.scene.json`, whose `lightmaps.desktop` set is
baked first:

```sh
./build_release/tools/bakery/vkr_bakery bake lightmap \
  --scene assets/scenes/fixtures/bistro_desktop_lamps_local.scene.json \
  --output assets/textures/bistro_lamps_local.vklm
./build_release/tools/vkr_harness profile \
  --case tools/cases/performance/bistro_vulkan_high_orbit_1440.case.json \
  --profile tools/profiles/performance-windowed-gpu-submission-vulkan.json
```

The bake uses its defaults (64 samples, 8 texels per metre, 16 for direct
light) and gave VKLM version 6,
`sha256:0de54f87bcec0c767f209eec67cfdea0a02a5c648ff963cd129684123910c056`.
The street walk uses `bistro_vulkan_high_street_walk_1440.case.json`, and
the budget lines `performance-windowed-gpu.json` with
`--pass-groups tools/budgets/desktop.pass_groups.json`.

| Case | `gpu.submission` median / p95 / max | Child medians / p95s | `frame.wall` p95 | Report |
|---|---|---|---|---|
| Orbit | 12.72 / 13.98 / 22.34 ms | 12.71–12.72 / 13.93–13.98 ms | 14.47 ms | `sha256:8f165e2a0d7352a733b47df344e9f1aafa1e68c7596eb6f382f8f58955f979d0` |
| Street walk | 13.08 / 13.94 / 24.07 ms | 13.05–13.12 / 13.88–14.03 ms | 14.36 ms | `sha256:bcff49a9bf8874f85287a0de4ea6d72dd6a4512486e4c641c2053325790dc2a5` |

Both cases meet 16.7 ms p95 `gpu.submission`, the orbit by 2.72 ms and the
walk by 2.76 ms, and `frame.wall` p95 stays under 16.7 ms. The budget is on
p95: the walk's worst frame takes 24.07 ms.

Budget lines, median / p95 in ms, from per-pass timestamps (orbit
`sha256:91bb52e269084c9290bfe452d62609e9dddb98205c8c206dad7c08fa74c7b7e9`,
walk
`sha256:a5abb42ce71c5bb1f82f490010d4f62ae37455dc85f2c27ea9bcc4abad0ccbbb`),
with each line's p95 target ([Desktop budget lines](#desktop-budget-lines)):

| Line | Orbit | Street walk | p95 target |
|---|---|---|---|
| Culling | 0.03 / 0.03 | 0.03 / 0.03 | 0.1 |
| Shadows | 0.07 / 0.72 | 0.00 / 0.49 | 0.8 |
| Visibility | 0.64 / 0.77 | 0.72 / 0.93 | 1.0 |
| Surface | 2.80 / 2.90 | 2.83 / 2.94 | 3.0 |
| Lighting | 3.65 / 3.83 | 3.74 / 3.90 | 4.0 |
| Media | 0.51 / 0.63 | 0.53 / 0.60 | 0.8 |
| Glass and blend | 2.04 / 2.99 | 2.32 / 2.63 | 3.0 |
| Reconstruction and post | 2.81 / 2.99 | 2.86 / 3.03 | 3.2 |
| Editor and debug | 0 / 0 | 0 / 0 | 0 |

Lighting is still the largest line, all of it `Lighting.Deferred.Fullscreen`
(3.65 and 3.74 ms median). Next come `FSR31.Upscale` (1.85 and 1.90 ms),
`GBuffer.Resolve.Fullscreen` (about 1.3 ms, which also reads the lightmaps)
and `AO.Evaluate` (1.0 ms). Bistro has no froxel fog volume, so the Media
line holds no `Froxel.*` pass. Passes can overlap, so the lines are not a
partition of `gpu.submission`; the orbit's medians sum to 12.6 ms.

The same session's runs of `425d514d`, the commit before the deferred
kernel change, gave 13.86 / 15.13 ms on the orbit
(`sha256:e90b9d30aceee3e687d4476c710ef448609b42f3c30f2a6ea29eba285c95e5ee`)
and 14.21 / 15.08 ms on the walk
(`sha256:85ebfe11205dba1dc3d2bb8445ebe5d68a0c1acc7fe7be6c16d1948ce7aa037b`);
`vkr_harness perf-compare` finds both faster after it, by 1.14 and 1.13 ms
of `gpu.submission` at the median, with no work-volume change
([ADR-019](019-bounded-forward-spatial-lighting.md)). Two earlier baselines
were accepted. The first baked-lamp one, on 2026-10-22 at `e7caca25` (the
same renderer as `425d514d`), took 14.22 / 15.79 ms on the orbit
(`sha256:7d3ed3c80add8a94457081ace595ef626887e465e7a6c393970685877d32ae93`)
and 14.21 / 15.08 ms on the walk
(`sha256:cfabf0a8fcf14a3323399b7b8c1f508fcbe3154a5ebdf6b06c7b43d27d09a534`);
its orbit is 0.36 / 0.66 ms slower than the same code later that day, the
spread between sessions. Runtime-lamp controls beside it, on the tracked
`bistro.scene.json`, gave 18.42 / 23.82 ms and 22.26 / 26.50 ms, within
0.2 ms of the first baseline, accepted on 2026-10-20 at `117122e6` on driver
26.6.3 with runtime lamps: 18.31 / 23.76 / 25.37 ms on the orbit
(`sha256:8c08b1eaef80a96fb1d2b8d625847bf7ed589cd5729b896751325a87864d09dc`)
and 22.43 / 26.68 / 35.49 ms on the walk
(`sha256:90b9cb9a0c9f5bf0157ddef60d7cf289fafed44c7ed669fda45662c8725f216d`),
with `Shadow.LocalMask` at 2.05 and 4.24 ms median and
`Lighting.Deferred.Fullscreen` at 6.74 and 8.14 ms. A speed claim against
this baseline still needs its own matched before and after reports.

### Desktop budget measurement

- **Cases.** [`bistro_vulkan_high_orbit_1440`](../../tools/cases/performance/bistro_vulkan_high_orbit_1440.case.json)
  measures steady cost and
  [`bistro_vulkan_high_street_walk_1440`](../../tools/cases/performance/bistro_vulkan_high_street_walk_1440.case.json)
  the tails: Vulkan, 2560×1440 physical pixels, a hidden window with
  immediate presentation and the High preset. Since 2026-10-21 both load
  [`bistro_desktop_lamps_local.scene.json`](../../assets/scenes/fixtures/bistro_desktop_lamps_local.scene.json),
  Bistro with its static lamps baked; a host bakes the set before it runs
  them, and a report whose `lighting.point.selected` is not 0 ran runtime
  lamps.
- **Profiles.** [`performance-windowed-gpu-submission-vulkan.json`](../../tools/profiles/performance-windowed-gpu-submission-vulkan.json)
  measures `gpu.submission` with five children on a clean tree, pinned to the
  RX 6700 XT; `performance-windowed-gpu.json` with `--pass-groups`
  [`desktop.pass_groups.json`](../../tools/budgets/desktop.pass_groups.json)
  reports the budget lines
  ([ADR-051](051-renderer-harness-and-evidence.md)).
- **Cases carry the preset.** A harness case reads no Graphics settings and
  has no preset field (`vkr_harness_child_apply_renderer` in
  [`vkr_harness_child.c`](../../tools/harness/vkr_harness_child.c)), and it
  leaves bloom and GTAO off unless it turns them on. The two cases spell out
  High gate by gate, so when `graphics_preset_gates` in
  [`vkr_graphics_settings.c`](../../runtime/src/vkr_graphics_settings.c)
  changes High, they change with it. Soft and local shadows, the draw
  distance, reflection probes and filtered far shadows default to High's
  values; fog, volumetric fog and subsurface scattering have no case field
  and do no work in Bistro. `VKR_SUN_RAY_SHADOWS`, `VKR_SUN_CONTACT_SHADOWS`,
  `VKR_BLOOM_DISABLED`, `VKR_GTAO_DISABLED`, `VKR_DRAW_DISTANCE`,
  `VKR_SHADOW_FAR_DISABLED`, `VKR_LOCAL_SHADOW_FADE_DISTANCE`,
  `VKR_LOCAL_SHADOW_ATLAS_LAYERS` and `VKR_RAY_QUERY_DISABLED` change gates
  without changing the workload fingerprint, which records only
  `VKR_SHADOW_EVSM` and `VKR_POST_TRANSFORM_CACHE`; budget runs leave them
  unset.
- **Host.** The Windows host's only display is 1920×1080; a hidden window
  takes the size it asks for
  ([`vkr_window_windows.c`](../../runtime/src/platform/vkr_window_windows.c)).
  Peer sessions keep the checkout dirty, so authoritative runs come from a
  separate worktree at the commit under test with Bistro's untracked assets
  linked in. They start on an idle GPU: a browser left rendering at 2 to 3%
  of an engine, under the harness's 10% authority limit, slowed a 14 ms frame
  by about 0.6 ms. One commit measured up to 0.7 ms apart between sessions on
  2026-10-22, so a speed claim pairs before and after reports from one
  session.
- **Limits.** One device sets the budget; RDNA 3, Ampere and newer GPUs have
  no native evidence ([ADR-083](083-supported-hardware-matrix.md)). The orbit
  and walk can miss content-driven peaks, such as many lights in view. A
  hidden window never presents, so a visible full-screen window's
  presentation cost is not in the measurement.

### Desktop budget lines

Each line of [`desktop.pass_groups.json`](../../tools/budgets/desktop.pass_groups.json)
carries a p95 target (owner decisions, 2026-10-22): the worse case's p95 in
the first baked-lamp baseline, rounded up to 0.1 ms. With the deferred kernel
change the owner lowered the Lighting target from 5.2 to 4.0 ms by the same
rule and kept the others. The lines overlap and peak in different frames, so
the targets do not partition the budget. The frame's margin to 16.7 ms p95,
2.7 ms on the orbit and 2.8 ms on the walk in the current baseline, is a
reserve that a feature claims only by the owner's decision.
`profile --pass-groups` writes each line's `p95_target_ms` and
`p95_margin_ms`, negative over its target; no verdict reads them.

| Line | Passes | p95 target |
|---|---|---|
| Culling | `Cull.*`, `Transmission.Cull.*`, skinning | 0.1 ms |
| Shadows | cascade, far, local and moments redraws | 0.8 ms |
| Visibility | `VBuffer.*`, `HZB.*`, `SDSM.Reduce` | 1.0 ms |
| Surface | `GBuffer.Resolve.*`, `AO.*`, subsurface gather | 3.0 ms |
| Lighting | `Shadow.LocalMask`, sun shadows, volumes, IBL, reflections, `Lighting.Deferred.*` | 4.0 ms |
| Media | sky, clouds, froxels, fog | 0.8 ms |
| Glass and blend | `Transmission.*`, blend, feedback copy | 3.0 ms |
| Reconstruction and post | `FSR31.*`, `Temporal.*`, `Post.*`, UI | 3.2 ms |
| Editor and debug | editor, picking, previews | 0 ms |

### Desktop budget levers

The owner ordered the levers: preset gates, then work inside the largest
line, then antialiasing, and dynamic resolution only if a fixed resolution
cannot meet the budget, since a changing resolution blurs the image in
motion. Measured 2026-10-21 against the runtime-lamp High cases (orbit
18.15 / 23.50 ms, walk 22.06 / 26.25 ms median / p95, the mean of two High
runs bracketing the variants, 0.1 ms apart or less), each variant a copy of a
case with one renderer field changed, all authoritative:

| Change from High | Orbit median / p95 | Walk median / p95 |
|---|---|---|
| Local shadows off (`shadow_local: false`) | −3.01 / −6.56 ms | −5.98 / −8.24 ms |
| Ambient occlusion off | −1.52 / −1.52 ms | −1.32 / −1.29 ms |
| Soft shadows off | −0.41 / −0.64 ms | −0.56 / −0.48 ms |
| Shadow quality Medium, filtered far shadows kept | −0.03 / −0.78 ms | −0.56 / −0.76 ms |
| Bloom off | −0.16 / −0.17 ms | −0.06 / −0.01 ms |
| Reflection probe off | −0.05 / −0.13 ms | +0.05 / +0.01 ms |
| Filtered far shadows off (`VKR_SHADOW_EVSM=0`) | +0.04 / +0.07 ms | +0.15 / +0.20 ms |
| View distance Medium (`draw_distance_scale: 0.75`) | +0.04 / 0.00 ms | +0.11 / +0.14 ms |
| SMAA T2x instead of FSR | −0.68 / −0.75 ms | −0.69 / −0.63 ms |

Each variant has its own workload fingerprint, so the changes are read from
the reports. No gate alone met the budget; local shadows off came closest,
at 16.95 ms p95 on the orbit and 18.01 ms on the walk, and took the shadows
of Bistro's lamps. Gates that cost 0.2 ms or less save nothing. On
2026-10-21 the owner kept every High gate and FSR over SMAA T2x
([ADR-052](052-vulkan-fsr31-upscaling.md)), and chose Bistro's static
lamps baked for the budget cases, which removes 4.0 and 7.6 ms at the median
and softens the lamps' shadows
([ADR-104](104-desktop-baked-lamps.md#desktop-budget)). On 2026-10-22 the
deferred lighting base kernel stopped carrying the local shadow inline
filter, which took 1.14 and 1.13 ms more off the baked cases' median and
0.86 and 0.80 ms off the runtime-lamp ones
([ADR-019](019-bounded-forward-spatial-lighting.md)). Lamps that move or
switch stay runtime lights; their remaining cost, mostly `Shadow.LocalMask`
filtering at 4.2 ms median on the runtime-lamp walk, belongs to
[local shadow architecture](../proposals/local-shadow-architecture.md) and
[lighting efficiency](../proposals/lighting-efficiency.md). Speed features
([asynchronous compute](../proposals/async-compute.md),
[variable-rate shading](../proposals/variable-rate-shading.md),
[GPU light binning](../proposals/clustered-light-and-decal-binning.md)) each
claim their gain against the baseline with matched reports, and a change to
the High preset's look is the owner's decision.

### Desktop 120 Hz line

The 120 Hz line (owner decision, 2026-10-22) is High at 1920×1080 physical
pixels on the RX 6700 XT: 8.3 ms p95 `gpu.submission` and `frame.wall`,
measured by copies of the two budget cases with only `resolution` set to
1920×1080. Measured at `a83cc070`
the same evening (authoritative, five children each), the GPU meets it by
less than 0.1 ms and `frame.wall` misses it by 0.2 to 0.35 ms, the open
gap; at 2560×1440 no preset comes near it without upscaling, which the
Vulkan pipeline does not yet have:

| Configuration | Orbit median / p95 | Walk median / p95 | `frame.wall` p95, orbit / walk |
|---|---|---|---|
| High, 1920×1080 | 7.40 / 8.27 ms | 7.55 / 8.24 ms | 8.68 / 8.56 ms |
| Medium, 2560×1440 | 12.81 / 14.09 ms | 13.13 / 14.02 ms | 14.45 / 14.36 ms |
| Low, 2560×1440 | 11.23 / 12.51 ms | 11.59 / 12.38 ms | 12.83 / 12.72 ms |

Values are `gpu.submission` (reports `802f79b3`, `1ca599b3`; `8df05b8d`,
`d3c41147`; `7fed5af8`, `89abad90`, digest prefixes); Medium and Low set
their gates as `graphics_preset_gates` does, with `VKR_SHADOW_EVSM=0`.
With baked lamps, Medium costs the same as High at 1440 within 0.1 ms. The
walk at 1920×1080 fails its `visibility.gbuffer.resolve_invalid` assertion
with one invalid pixel, before the deferred kernel change as well
(`425d514d`, 8.27 / 8.99 ms), an open defect; the 2560×1440 cases pass
it.

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
has no local reflection probes yet (the desktop run also had SSR, removed
on 2026-10-12), and the desktop interior is lit
by lamps the tiled one does not draw yet.

### Dynamic light evidence

Release editor, M1 Pro, 2026-10-06, the toolkit test level at 22:00: a
dynamic spot light casting shadows over a brush pillar and an unshadowed
dynamic point light, added through `vkr_mcp`, light the room on both
pipelines, the desktop one on Metal before its removal, with the same pool, pillar shadow and tint, over the baked lamp
groups on the tiled one. `test_point_light_limit_keeps_nearest`,
`test_point_light_limit_holds_and_fades`, the time-of-day lighting test and
`test_local_shadow_fades_out_when_light_stops_casting` cover the limit, its
hysteresis and fades, and the static-light filter. The fades and the
tent for all four shadowed lights have no native capture or timing yet; the
0.1 ms tent cost of decision 11 was measured with two full-filter lights.

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
`tiled_bistro_baked_native`, whose one indoor probe (then a placeholder
cube with specular intensity 0, diffuse 1) is in view for much of the orbit. With pass timestamps
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

Per-pixel choice for draws that straddle a probe's half-influence surface,
Release, M1 Pro, 2026-10-13, `tiled_bistro_baked_native` with
`performance-windowed-gpu` (authoritative, 5 × 300 frames), alternating the
build before (`c04f329a`) with the change: `Tiled.Opaque` p50 rose from
10.46 / 10.43 ms to 10.69 / 10.66 ms in one series and from 10.30 /
10.33 ms to 10.61 / 10.67 ms in another, about +0.2 to +0.3 ms. A build
that carries the per-pixel branch in the shader but never selects it
measured 10.49 / 10.52 ms against the change's 10.65 ms, so about half the
cost is the branch's register use and half the per-pixel work; Bistro's
merged meshes straddle the café box and cover most of the frame, so a
separate per-pixel variant would recover little. The café capture
(`local_shadow_bistro_indoor_capture` on Metal) changes visibly: the café
probe's box (x −13 to −2) covers only part of the room in view, and since
`bistro.scene.json` lost its diffuse volume those interior surfaces carry no
baked light, so outside the box they reflect the unscaled sky and automatic
exposure darkens the frame by about half; per draw, the merged meshes had
taken the café probe for every pixel. The street capture is unchanged
beyond the sky's run-to-run variation, and a variant of the encode rule that
sends fewer draws per pixel produced a byte-identical café capture.

### Refraction measurement

Release, M1 Pro, 2026-10-06, the build before refraction (`1c64f848`,
through `VKR_SHADER_CATALOG`) alternating with the refraction build on
`tiled_bistro_baked_native`. With Bistro's glass unchanged no frame declares
the copy. Pass timestamps (`local-windowed-gpu-single`) put `Tiled.Opaque` at
7.94 and 7.77 ms median before and 7.98 and 7.76 ms after, and `Tiled.Blend`
at 0.50 and 0.49 ms on both
(`sha256:913ce53f54ec4d7c27a452a415a9a069f519078979dde8839ce1f2e11166a987`,
`sha256:beb21f3659ce3f71254a8ef1e191e84d8c20c1cb3fe3354613b1e770327bce6e`,
`sha256:8f75418a711b06a89207e02e02791c5c8c69f57f7da44dd4e545888d8f34c6f3`,
`sha256:47206afc8fd138923a7d8e13093538014cd0622c2c2693e5db3713fc1e145371`).
`gpu.submission` (`local-windowed-gpu-submission-single`, the refraction
build first in each pair) took 11.68 / 15.97, 11.78 / 16.53 and
11.88 / 16.52 ms median / p95 before and 11.62 / 16.01, 11.74 / 16.00 and
12.21 / 17.29 ms after
(`sha256:f031a1538f453b89f1a13754b721c6e86469c22b2cd0eed3241275af0cfe9a60`,
`sha256:7f1212b68dadb5914d2aa5fc0b0600c51c1f4622f47eacdfb226c645e59d3493`,
`sha256:217ee5f99e1c5839b7248adfa9f5de683562666dc87836b6aa19bd89819bc75d`,
`sha256:b226cec5a66f809dd251121bf9b34f3eac995c9c87e6c292766ec57b2c2b3970`,
`sha256:bb70389641338e1249c547969cb63dd629f90d9cfb293ae56bf1343c6cd0e308`,
`sha256:59168f18f27296125bba9d6045160cddc82717c5c93dea31504e83b599483c9f`).
Runs with the refraction build second showed it about 0.15 ms slower at the
median, the drift between consecutive runs of one build; the reversed order
removed it. Snapshots of `tiled_bistro_glass` and
`tiled_bistro_baked_capture` differ from the build before in at most 3
pixels by more than 2 of 255.

With Bistro's glass temporarily given a 2 cm world thickness and
attenuation, and its six normal-mapped panes a roughness of 0.45 without
their roughness texture, the copy at 2560×1440 took 0.28 and 0.29 ms median
for its base level, 0.05 ms for the first reduction and 0.01 ms for each of
the four smaller ones, about 0.37 ms in all; `Tiled.Blend` took 0.51 and
0.50 ms against 0.51 and 0.50 ms before
(`sha256:5e26965670a6771fb1677bb9632efab0c5e8f521b4beee2c1adb56a5c3a9306e`,
`sha256:471fee056bb56d0f45ae7d94192af57afe046332ff5fc808d4beed7e9ab6a92f`,
`sha256:7d0c4842e3e78b08b864e094d8c5b1d09f20dd97923446a3f8799fa3ee72620e`,
`sha256:1300aa68516db82531428419c8112cbec291b83c809e3676302b77e457d2b502`).
The base level is bound by its read of the full-resolution image: four
taps over the same 2×2 footprint took 0.28 and 0.31 ms
(`sha256:cd2d4ab74551b8a4c0c9654d96163382904c8b82247ee9a386705eda8a83d6ee`,
`sha256:ac083f193e42f25459f09ac08e3f4def0779dff446fcfed456340eebeaf5e41d`).
At 1280×720 (`tiled_bistro_glass`, `local-offscreen-gpu-single`) the copy
took 0.04 ms and 0.01 ms per reduction. Captures of the café windows show the
rough panes blurring the interior and the smooth ones bending it slightly
(`sha256:df6451c13b7506d6249ced3704be38eb75bcacc683fe81f6bd08ed7e4089eb51`
before, `sha256:bcfe529c3dc64ad86eb8e5b59229ad57e106325984ecf00eb7f7a3453ec889e8`
after), and a Metal API validation run of that view with the copy passed
(`sha256:5ddd829c21b0b89f3fb439fa46db9149af5b37abc5506010a7ea8363f8dd8208`).

The rough flag, measured 2026-10-07 the same way against the build before it
(`a9e10cba`), four alternating runs each: with Bistro's glass unchanged,
`Tiled.Blend` took 0.49 to 0.50 ms median and 0.84 to 0.86 ms p95 before and
0.50 to 0.51 and 0.90 to 0.92 ms after
(`sha256:c8893a84f97adcdaad35b7638a218cec50a3a126d3e03cf4003ac71099689498`,
`sha256:433e907cfb7e5cebe9319b71b9c6c2245ba63bfcca84eb03a88184f641916b05`),
and `gpu.submission` 11.62 to 11.66 / 15.99 to 16.20 ms before and 11.64 to
11.70 / 16.01 to 16.11 ms after
(`sha256:e63af818bcd7c926efefb957ecd559af0914adf3ffc1b4ad2f48b4a16188bd57`,
`sha256:eb2e4f405fb0d3306db973c96870f073b5136bd457f035b5d3ac3cf5988f194a`).
`tiled_bistro_glass` is unchanged pixel for pixel. With the six normal-mapped
panes temporarily given a roughness of 0.45 without their roughness texture,
and every pane thin, the rough windows and lamp covers blur the light behind
them while the smooth panes stay sharp
(`sha256:72f0f3f575becd84d98bac4547e51c8b3154d11e65e73f4be12ce9702538e284`
before, `sha256:cc0d08a622e71f878d2d81a66bf429381c7471698207826b61886936756edf7f`
after), and a Metal API validation run of that view passed
(`sha256:0c25064f855ac655f715a10013053fe7f9bd2ac84ceab927c0603138c9a5db50`).
`tiled_bistro_baked_capture` takes one of two cloud states from run to run on
either build, so its comparison holds outside the clouds.

### Depth pre-pass measurement

Release, M1 Pro, 2026-10-07: HEAD `eb2d05df`, copied with its shader
catalog (`VKR_SHADER_CATALOG`), alternating with a build that skipped the
pre-pass loop, two runs each, then the same against the committed change,
which also removes the depth-only pipeline and its depth state. Shading keeps
`LessEqual` with depth write, so the later of two coplanar opaque surfaces
still wins. Pass timings use `local-windowed-gpu-single`, the others
`local-windowed-gpu-submission-single`.

| Case, metric | HEAD median / p95 | Without pre-pass |
|---|---|---|
| `tiled_bistro_baked_native`, `gpu.submission` | 11.65 / 16.06, 11.67 / 16.15 ms | 11.09 / 15.59, 11.09 / 15.66 ms |
| the same, committed change | 11.63 / 16.01, 11.65 / 16.03 ms | 11.21 / 15.84, 11.15 / 15.69 ms |
| `tiled_bistro_baked_native`, `Tiled.Opaque` | 7.75 / 12.18, 7.77 / 12.09 ms | 7.21 / 11.59, 7.22 / 11.62 ms |
| the same, committed change | 7.76 / 12.07, 7.77 / 12.05 ms | 7.24 / 11.66, 7.22 / 11.68 ms |
| `tiled_bistro_baked_dynamic_native`, `gpu.submission` | 13.84 / 21.78, 13.88 / 21.78 ms | 13.43 / 21.38, 13.48 / 21.57 ms |
| `tiled_bistro_baked_dynamic_native`, `Tiled.Opaque` | 9.82 / 17.50, 9.79 / 17.48 ms | 9.45 / 17.19, 9.45 / 17.12 ms |

Three HEAD runs report `incomplete`: they fail the case's minimum of one
cascade-0 indirect command, as runs of either build did in earlier series,
and their 300 samples are valid. Reports, HEAD then without the pre-pass, in
table order:
`sha256:246de3a3337f382eb2e71b638f5e68358e43d225456d5c73eeb2e733b64c24c8`,
`sha256:a9bade5389fd35563c1671f87f5436702ad1344616728bf4407c361739bd116c`,
`sha256:4ab7605f63bc3426b4a45526c24fef9edf925d09ae7fc1709e88360643d7706e`,
`sha256:5609e8978b30a7465e76d23fbbf4204c6dfe76f03623e54871b584559014fd7a`;
`sha256:64210691928b2447c42a816b3b3ef85447848fae83f0ddc60d4a974f61459298`,
`sha256:65a9a9e483373675d0cd65be998d41e76f96bf1dd63eccf3b04ebd089328e58e`,
`sha256:aa1b1212741db6f4ee7b22635703cea3fbb19aeba457c5bb365bde1d87397295`,
`sha256:fc0ac619c91aeaddde6093c9b556409d3345407556e4d9455b927b40890d4229`;
`sha256:523008b72d70d25cd69cda002b5d6e5a7a2be025648f72aa7e9d1046f0541e01`,
`sha256:40284af7b0b8d8cc58ee603e216454357aa83490ce12ab5d742c32c0a36028a0`,
`sha256:fe1103426370bf3625e336ce463e09b26efbbe69b9406d28e671d3a87ead0735`,
`sha256:dbe155a847747cc7ce901f257105a54a2c3fe9c1269b2b02546c0535bcb2b827`;
`sha256:233cf6ee90ea48d9e7925e5bab4bdbf6407fc546b3b4f695672f804462591a78`,
`sha256:6d7cdef09146ffd496594e235cd4948a786387d25c2c055e1f2db3350f36eb2f`,
`sha256:b61ab7d2b1ee6c16032b182570648a513ee2f3b67101d5adaf5e00ad283e62ea`,
`sha256:a68aab936674aaa12b290136c6aaf51545ee4d9673029d1f48251faa3ff6eb34`;
`sha256:7c9ad3ac8d4207a8b5c3330ad190378f16a215a43072d0dc8828b731bcb00c26`,
`sha256:a80884ef6eaaf7764d189b2191eafe3def4d00f18c2b424b543fec3170b3a0e2`,
`sha256:31fe053b0ee9cc52f5cb59190a2ae7e4a76a6c162966610d6f5f8675547b2b6f`,
`sha256:7c9cf6a89729bcf82cb3046b258915541511900a0601f5334fbe6ca8f1b94bcb`;
`sha256:8ac5dbff3777844af6afc85ed5b44c6f3f0ab0582e6970d19f3433e1ea2fa755`,
`sha256:63ed511646d35c2b3539e0d2c2873074c984e9a457f83dd7e2b5ba7184bd4349`,
`sha256:e20c4b07f0fd73d6644794a3040cb8b905d564ad48ff5d30f6e4695ec4b7eeb8`,
`sha256:8b58451796c58adae95a7adc012856345e9a37ee9745a39d7f56befa6d7201cc`.

`tiled_bistro_baked_capture` (`local-offscreen`) keeps HEAD's depth and
differs in 1 pixel by more than 8 of 255, within the variation between two
runs of one build (up to 3 pixels)
(`sha256:d6439365f2e702a19020aba8a86e111e2e56391d5afc743fcfb6c2946fdfbc41`
HEAD,
`sha256:e83d70b6b1e7c68a10e70f7b2ab047db9740755364668275954296de24b65d11`
committed), and `tiled_bistro_glass` is identical to HEAD pixel for pixel
(`sha256:441cb2f024b9db28384fcc9e047b859f267b8775bb82c88aa9e4cffdc403ffe7`,
`sha256:56d605a72218811a87baadfcc10f6bdbade4de18e89fac81fbbf35d2ddf8fe2a`).
Builds that differ only in shader comments or in the pass's clear colour
switch 2 pixels of that view between two states, up to 6 of 255 apart, so a
difference there alone does not show a change. A Metal API validation run of
the baked view passed
(`sha256:b52262e5cbd9603a329f0428096ffd532135073a297fb3cfa9990b3c9dc1ba6c`).

Before the removal, `[[invariant]]` on the tiled vertex position with
`-fpreserve-invariance` was measured as the guarantee that the pre-pass and
shading compilations lacked. In the linked library only
`vkr_metal_tiled_vertex` then compiles without fast math; its position
rounding changes depth within 16 ULP almost everywhere and moves sample
coverage in 50 edge pixels of the baked view by more than 8 of 255, and
`Tiled.Opaque` took 7.83 / 12.24 and 7.85 / 12.25 ms, 0.06 to 0.08 ms more at
the median, with `gpu.submission` at 11.74 / 16.30 and 11.69 / 16.19 ms
(`sha256:b6c06088edd944bc0c3ce9105418b65dcf75c58f55de4e15fee2d4016408a872`,
`sha256:983b4c54e2989b02713bee4d42227711e489dfda589adaf2900e769bada4cd7a`,
`sha256:050ccf547442aaf385c861fe42a18a9a74e4238f06960f348e46f5c5e5f294d3`,
`sha256:e871dc11d2ef85da0699b12de1b5b6456c96de2e11db7d266d30d5105940095a`).
With one pipeline per opaque surface no pass depends on two compilations of
a position agreeing, so it was not kept.

### Base pass measurement

Release, M1 Pro, 2026-10-07: HEAD `cc35eb0e`, copied with its shader
catalog, alternating with the build whose alpha-tested fragments test their
coverage after the base colour sample and before the ORM and normal samples
(`vkr_metal_tiled_base` in
[`tiled.metal`](../../renderer/src/shaders/metal/msl/world/tiled.metal)).
Two pairs ran HEAD first and one pair the new build first, because the
second run of a pair measured up to 0.3 ms slower in either build.
`Tiled.Opaque` median / p95 (`local-windowed-gpu-single`), the pair with the
new build first last:

| Case | HEAD | Discard before the other textures |
|---|---|---|
| `tiled_bistro_baked_native` | 7.32 / 11.66, 7.32 / 11.68, 7.47 / 12.23 ms | 7.18 / 11.67, 7.18 / 11.66, 7.30 / 11.69 ms |
| `tiled_bistro_baked_dynamic_native` | 9.50 / 17.15, 9.45 / 17.28, 9.53 / 17.24 ms | 9.56 / 17.32, 9.44 / 17.05, 9.30 / 17.11 ms |

The baked orbit's median falls 0.14 to 0.17 ms in either order; the
dynamic-light orbit shows no change beyond the order drift. `gpu.submission`
(`local-windowed-gpu-submission-single`, `tiled_bistro_baked_native`) took
11.42 / 16.07 and 11.39 / 16.38 ms at HEAD and 11.66 / 17.08 and 11.54 /
16.43 ms with HEAD first, and 11.24 / 15.93 and 11.50 / 16.88 ms against
11.22 / 16.08 and 11.21 / 15.91 ms with the new build first: the frame
changes no more than the order drift. Reports: pass timings, HEAD then new,
`sha256:df029057bbd08c418eb718d0e730a40bb930c549c3536e4d61b82cf2d0958dcb`,
`sha256:b854c38c198f3d811eaa3ed0be54b329d52f75f8c6648a93f2cf61c843462fb0`,
`sha256:d224b47fe7d7f07fcc0109c74f460ee6522d0c37048564c0face669a418e7c81`,
`sha256:3fae3cfc53efff69428c7043592352c3062e67bd19fe66073eb1384ecbf4ded8`,
`sha256:4c1b0069a950191d7618cae240b974381f6a9c0f5ad358e85759b86676af0907`,
`sha256:9f78d4acac20c8937baf8d9c155649b243392c02019fcf5a74e2bfaba9ec43c3`;
`sha256:8783f223abaeb00c791043b8c9544d3227092c797328dfb17e951e4e377d5940`,
`sha256:25af433798c60517a4c0d388773647173659d898a77d6d56b07e31639a0201e1`,
`sha256:f1651c01723fb64b1422494103536062a5dcfbe7827ac05d4c33bc52e620eafa`,
`sha256:64f6aa25f6d052470e40478efe900818265779ffced271bee4faa4a8f79bdf72`,
`sha256:fde603268c3ecb4db7b5f674f49798f67321e8c6346050818febdc1ebda8101b`,
`sha256:67721a4b81137494c10adec4a79b90d1a45eec90bb369a21d6073cb3aa5c05ec`;
`gpu.submission`, HEAD then new,
`sha256:790a320c8c45aa2e38c5d7ba7ba3e15a71dcb614c458b9d070fcb928dfcdecf2`,
`sha256:2702dcfc98119b6cb7e7c4a5893cc6596edf2e88e5adefdbce4adc5694c33b29`,
`sha256:eb4a661facb3d24ee8aefc12b9bef31c785234d5d71d3281c3c2725284a856e6`,
`sha256:30b2bf09da38ff63230d8c0517abab8e51b2c3036d36eae6742fe62e5acc6402`;
`sha256:4c3c1ffccab938334323de4acd2750770946454290e44b275274c967a06a6cb0`,
`sha256:6ffbc24ab7f991fc7732e51df60ac90defbe9cf5f7a5fdb275e8f32687255b25`,
`sha256:3793e6931870205cc9ab1a869b311ff6ef91944ab2232ceba175d30639dc2ccc`,
`sha256:1be9ef210424d60f1e1d75eba978cf3bbd468f7f2299feb1e75490cc65f339d2`.
`tiled_bistro_baked_capture` and `tiled_bistro_baked_dynamic_capture`
(`local-offscreen`) differ from HEAD in at most 2 pixels by more than 2 of
255, within the variation between two runs of one build
(`sha256:696a051fc9242eb3fd0874335b660f2c8beb26154ec6287473ea79861e623065`,
`sha256:cd2e0cc02b6162af9c06b5948dc071147a8169f094d3358af9bb9db4dac9f3cf`,
`sha256:047ac0a373f961067666b7cc7dfca69ef91b23cda6554856c700b65a43367caf`,
`sha256:e03f1079d20b0f520f18c2d4e4f6879fc450e7c0ef31fc10d919c2f6e07ef737`),
and a Metal API validation run of the baked view passed
(`sha256:9bb38702be5677517c9bf99b4fc64a8f89aaadcd8c1f88f13fadd729aa90f9e8`).

Ordering the camera view's alpha-tested draws near to far was measured the
same way against that build (`08e6077d`) and not kept. Classify counted the
draws of each alpha-tested bucket in 32 distance bins, three per doubling of
the distance from the camera, and encode placed each draw after the nearer
bins of its bucket. The baked orbit has 56 to 162 alpha-tested draws in view
(112 at the median). Median / p95, the third pair with the new build first:

| Case, metric | Before | Near to far |
|---|---|---|
| `tiled_bistro_baked_native`, `Tiled.Opaque` | 7.21 / 11.64, 7.18 / 11.73, 7.18 / 11.71 ms | 7.26 / 11.73, 7.27 / 11.80, 7.26 / 11.79 ms |
| `tiled_bistro_baked_dynamic_native`, `Tiled.Opaque` | 9.41 / 17.17, 9.49 / 17.24, 9.58 / 17.10 ms | 9.45 / 17.26, 9.46 / 17.27, 9.45 / 17.30 ms |
| `tiled_bistro_baked_native`, `gpu.submission` | 11.48 / 16.64, 11.23 / 16.11, 11.25 / 16.12, 11.19 / 16.03 ms | 11.21 / 16.02, 11.24 / 16.29, 11.23 / 16.17, 11.25 / 16.05 ms |

The `gpu.submission` rows are two pairs in each order. Reversed to far to
near, the same bins left `Tiled.Opaque` at 7.24 / 12.40 and 7.24 / 11.83 ms
against 7.23 / 11.79 and 7.27 / 11.79 ms near to far, so on Bistro the order
of the alpha-tested draws does not change their cost. Captures of the baked,
dynamic-light and glass views stayed within the variation between runs of
one build. A pass that lays the alpha-tested depth before their shading, so
that shading tests `Equal` without discarding, was not built: the
[anti-aliasing measurement](#anti-aliasing-measurement) already laid that
depth before shading, where the depth test rejects hidden alpha-tested
samples, and it cost 0.5 ms more. Reports, before then near to far:
`sha256:b5a896418e0f83d8db760d69d0b9576af3cd48d74beab464f80fa663461c80e5`,
`sha256:34d41ed76923fc9caf5ee603485f6915180c0fc50ad1911b5cdc28bf1cf227a2`,
`sha256:ab27a6cbf9942cdbe82a03df503c067d156bfa1f154a10dbf593375a0ebd9b4d`,
`sha256:da465eccdd11f803d522e2499a541d8e98e2d7693f4d09b0502afd4ddb06b800`,
`sha256:46d07c732eb68b2a8ea9bda0568474a5ad82569a4ed83221a54a4c08edbd8bc1`,
`sha256:42d5e9b73da5517e8366142852a50a174093b96b9d00aecc94c8e4750314bbd0`,
`sha256:ff1f2c307dc7bd8c22f366cd8cb792539d9aaf6d45882f1f1b6f5f5f46cb873f`,
`sha256:6ce66e0a7932a22301c78762ab646458de7c0c046833a6a8a52062a0082ec6e1`,
`sha256:409ce9e02a31899ca744307e38bf1f9a3d849cdff5f7554cb3591b00614e2bed`,
`sha256:79a26c7960ce70b88ebb914ebc5b09fb11a2ffd30899e390ef6e7b554a51ea49`;
`sha256:4d75665cbba96de1fc495e58a11785c8f7fd386c341d8d0b2183427ab1bfde97`,
`sha256:b88a103587b591c65ce6066da3d893ce78948280eb696c024f84c223d940f492`,
`sha256:f16f48decedfa993171813f181524abfab719508acbb9b896c07b052e95b6bb1`,
`sha256:8d0358e509c96a8edf1849f115769a3a4ae07ef145ce646128c165216f6a8150`,
`sha256:c10d213e4be2f80f16129e2a3782f9f3b83ded33b9bcc4f419f8b7fdf99cf25b`,
`sha256:2b58c0bf07ea3c08086984db3f70825c930ab82a7673d6c90b03184df68867d6`,
`sha256:1ef5347834360fe8f8b9baf44d6e34ed43b0021df3cd356e67a047ca55c7312c`,
`sha256:88b7890f413b16af2d7f2f67b192d65a78da48164585f5f735bd7860d09b8ac9`,
`sha256:c45c6d8e7ff72a4662c12b2ef9605052c1cb896229042aa25061204be7c12343`,
`sha256:f0cbd05ce4ab088e269b6c95f9ff13e8db8eddb59427fb000536569e51cd8c2c`;
far to near
`sha256:df0d77e75bf4c3dfc5982d57061a4454b156ff24b4823a03eca63e642cafa551`,
`sha256:2e1617a3d20929ef6f68d677bfb4f7cc8e187da2f39eee954c957a5b318f22f4`
against
`sha256:f9fe3f1a8756121f109302d3e54d4cd5e2a33c17d2f9209b773151f7fe877ee0`,
`sha256:d6897158653649b0e1087ebb7ae1e4146927879c73232d5b0b85bd302756b8df`.

Carrying the forward vertex's colour, normal and tangent at half precision,
which shrinks its outputs from 96 to 74 bytes, was measured the same way
and not kept. It tested whether vertex outputs limit the widest views,
which are bound by geometry (drawing half of every index range lowered
their `Tiled.Opaque` p95 by about 4 ms, see the
[tiled pipeline proposal](../proposals/tiled-pipeline.md)): a tile-based GPU
writes each vertex's outputs to memory and reads them back for every tile. `Tiled.Opaque` median / p95 on the baked orbit took
7.27 / 11.95, 7.21 / 11.75 and 7.18 / 11.71 ms before and 7.56 / 11.89,
7.50 / 11.83 and 7.29 / 11.84 ms with half outputs, the last pair with the
new build first; the dynamic-light orbit 9.55 / 17.18, 9.52 / 17.07 and
9.40 / 17.05 against 9.50 / 17.05, 9.57 / 17.15 and 9.47 / 17.08 ms. The
`gpu.submission` series varied from 11.1 to 12.5 ms median in both builds
and shows no difference. Half normals also moved 7 isolated pixels of the
glass view by more than 2 of 255 (at most 15). Output size therefore does
not limit the forward pass on the M1 Pro. Reports, before then half:
`sha256:b28e9fa08dc39bdf6d946970d105fee722e3d5a47c1d353f8bdd185f37a78205`,
`sha256:60b48c628bebecaf5dea193f6f6deb91fe4009e1b8d46300bb6003e2cfdbb70c`,
`sha256:382c306a4c87483227a4354806ba180184f95b8506f822e9587b530dc7107d86`,
`sha256:f308e3c3446148842d63f1f5d28a54f9a8d5bafbf47e969f4d7d3be4e074c0a6`,
`sha256:fdb3ede9b8847ee246f02b663e66a37c5d44bf0de421c686cbe540631d1ab04a`,
`sha256:881b7d4439853e185c34999147723a54aad5ad21e3f8c2f2850577f1b4bf29fe`;
`sha256:6754b97f28884adce0bab3716223378c69cb96feb0b4466174fea30c2e97a754`,
`sha256:6b4c0b4b893e9cf2273d3e0b4591cfc1af3d71f767c660acc6d398dec3067f3d`,
`sha256:f18363046d69e57f8b935c54b3d52face1ac616408ddae534498ddec9d01ff31`,
`sha256:170e29227e38c83718a65e9f699df543027dc16ced82dcd48b859ae996766c92`,
`sha256:5880dde2bddfc2f09bc731c68439c1fabcc7e07ac332c85cb346a9b6032c218e`,
`sha256:b1175eeeae1166519252f5c349f8af1fa902d540ed26fde2dc0ec6642405c412`;
glass captures
`sha256:eab44f68831ad8c89595942ae3d57d64841bae39ab44a0573a696f0e146e2a9f`,
`sha256:d0331a66931e7a4c7a097db4bfeb1191da0859243d0290341ca5566949cb4564`.

### Opaque pass registers

The opaque pass is latency-bound at low occupancy (ADR-129), so its forward
fragment's register count sets most of its cost. The Metal Toolchain
compiles a pipelines script (`.mtlp-json`) to M1 Pro binaries offline
(`applegpu-nt -arch applegpu_g13s`); the fragment metadata's first table
holds the 32-bit registers per thread, calibrated with shaders whose live
values are known (8, 16, 24, 40, 76 and 108 registers for 4 to 96). On the
lightmapped Bistro street view (2560×1440, MetalFX at 0.6), shaders with
fewer registers ran at proportionally higher fragment occupancy down to
about 80: 104 registers 19.5%, 88 22.8%, 80 25.0%, 44 35.0% (features
compiled out for the test, so their times also lost work).

Before 2026-10-30 every forward variant used 104 to 108 registers, the same
with or without dynamic lights: three parts of the shader each reached about
88 to 104 with the rest live, the sun's cascade filter (its 16 taps and the
PCSS search issued together), the shadowed light loop, and the diffuse
volume and environment tail. Three changes lowered the variants to 92 to 96
registers and the opaque pass's fragment work, measured over 2 s Metal
System Traces (M1 Pro, Release, local and not authoritative):

| Change | Lightmapped Bistro | Bistro, dynamic lamps |
|---|---|---|
| Before | 3.84 ms, 19.5% occupancy | 7.42 / 7.57 ms, 19.0% |
| The sun's cascades sampled before the material is read (`vkr_metal_tiled_early_sun_visibility`); each diffuse-volume probe's SH evaluated at the normal as read | 3.52 ms, 21.7% | 7.28 ms, 19.8% |
| The cloud chain sampled from a cube (ADR-074) | 2.29 ms, 20.9% | 5.90 ms, 19.9% |
| A shadowed light's normal offset from its first view, without a second face's view; lights below `VKR_LOCAL_LIGHT_CONTRIBUTION_CUTOFF` skipped, as in the desktop pipeline | 2.28 ms | 5.64 ms |
| A shadowed light's face projected from its offset along the face's axes, without the face's matrix (the shadowed loop's registers 88 → 80); frames whose scene binds no diffuse volume take a variant without its lookup (`VKR_METAL_TILED_NO_VOLUME_VARIANT`, the general forward variants 92 → 76 registers) | 2.10 ms | 5.64 ms |
| The surface's diffuse, specular and emissive totals in half precision, as the colour target stores them (every forward variant 4 registers fewer) | 1.97 ms, 25.5% (2.10 to 2.12 ms, 22.9% before; one pair of runs measured both at 2.28 ms) | 5.64 / 5.65 ms, within the spread |

The early lookup gates on the geometric normal facing the sun in place of
the normal-mapped one; a surface whose geometric normal faces away lies in
its own shadow in the cascades. Moving the diffuse and environment light
ahead of the direct light raised the counts by 4, and a SIMD-uniform light
index did not change them. Captures of the last row against the build
before it: the lightmapped Bistro identical, Bistro with dynamic lamps at
most 1 of 255 (`tiled_bistro_capture`, `tiled_bistro_baked_capture`). The no-volume variant's captures equal
the same binary's with `VKR_TILED_NO_VOLUME_DISABLED`; the face projection
moves 6 pixels of Bistro with dynamic lamps by at most 7 of 255 (76.4 dB),
from rounding at shadow edges. Its timing on that scene, 5.64 against 5.78
and 5.79 ms off, is within this machine's spread.
The half-precision totals move both captures by at most 2 of 255 (65 to
67 dB), from rounding.

Sharing the sun's cascade filter across each 2×2 quad of fragments, each
reading the quarter of the 16 grid taps and 8 PCSS blocker taps toward its
own corner and taking the quad's sum, was built and measured on 2026-10-30
and not kept: each fragment read 6 taps instead of 24, but the opaque pass
took 1.96 to 2.26 ms on the lightmapped Bistro against 1.97 to 2.28 ms for
the same build without it, and 5.62 against 5.64 to 5.65 ms with dynamic
lamps, inside the spread. Sampled before the material is read, the taps no
longer set the register count, so they cost little, and sharing them would
have made the penumbra constant across each quad.

Deferring the dynamic lights to a full-screen draw at the end of each opaque
pass, which would read the surface from four memoryless targets the forward
draws write (base and metalness, the two normals, depth and roughness, the
material row), was compiled and counted on 2026-10-30 and not built: the
forward draws without lights still took 92 registers, and the light draw 64
with unshadowed lights, 80 with shadowed ones and 116 with rectangle lights.
The shadowed loop's own 80, for its shadow view and four compares a light,
would raise occupancy by about a seventh for the 3.6 ms the lights cost on
Bistro with dynamic lamps, against four more targets in the pass (the texture
demand target alone costs about 1 ms, see `tiled.metal`) and a costlier draw
for frames with rectangle lights. Captures against the build before these
changes are in ADR-074's tiled chain evidence.

### Sun contact shadow measurement

M1 Pro, Release, Bistro plaza looking down at sunlit paving
(`tiled_bistro_plaza_timing_sun_contact_{off,on}.case.json`), 2560×1440 at
0.6 render scale with MetalFX, `local-offscreen-gpu-repeated.json`, five
repetitions of 300 frames each, dirty tree, not authoritative, 2026-10-16:

| Shader arrangement | `Tiled.Opaque` p50 off → on | `Tiled.Atmosphere` p50 off → on |
|---|---|---|
| Sun light kept from the start of shading | 13.71 → 14.97 ms | 0.22 → 0.78 ms (world position per step) |
| The same, the sun variant writing zero | 13.71 → 13.60 ms | 0.22 → 0.27 ms (march skipped) |
| Visibility at the start, the sun lit after the local lights | 13.61–13.73 → 14.40–14.61 ms | 0.22 → 0.56 ms (linear depth per step) |
| Sun light packed in one register from the start; 8 independent steps | 13.58 → 13.67 ms | 0.22 → 0.40 ms |
| The same with the light and the cascades' visibility in one register, interleaved, machine in use | 14.41–14.58 → 13.99–15.13 ms | 0.22–0.23 → 0.43–0.45 ms |

The second target itself costs nothing; three values kept live through the
other lights' code lower the opaque pass's occupancy. The shipped
arrangement is the last: the sun is lit first, as without contact shadows,
and its light without the cascades travels to the return packed with their
visibility in one register (`vkr_metal_tiled_pack_sun`: three 6-bit
mantissas under a shared exponent and 8 bits of visibility), which only the
correction reads. The march takes eight steps whose depth reads do not
wait on each other, and skips pixels whose sun light is under 2% of their
light. Lighting the sun after the image-based light cost the base variant
2.5 ms and was dropped. The last two rows' pairs were interleaved (off, on,
off, on). In the second-to-last, a disturbed first run (14.30 ms off) is
left out, and the passes add about 0.28 ms, within the shadow quality
proposal's 0.4 ms. The last row's runs were disturbed by other GPU work and
bound the opaque cost only within about ±0.5 ms. Plaza captures change
3,675 of 921,600 pixels at 1280×720: contacts under hedges, bollards, curbs
and window frames.

Authoritative windowed timing at `fb52f7dd`, clean worktree, 2026-10-17:
`tiled_bistro_plaza_windowed_sun_{off,contact,sdf_replace_all}.case.json`
(hidden window, 1280×720 at 0.6 render scale with MetalFX, immediate
present) under `performance-windowed-gpu.json`, five repetitions of 300
frames, interleaved off, contact, replace-all, off, contact; every report
passed and was authoritative. p50/p95 in ms:

| Run | `Tiled.Opaque` | `Tiled.SunShadow` | `Tiled.Atmosphere` | Frame wall |
|---|---|---|---|---|
| off (`00a83d`) | 9.724/10.667 | — | 0.346/0.703 | 17.26 |
| contact (`00b1a4`) | 9.707/10.932 | — | 0.449/0.970 | 17.41 |
| SDF everywhere (`00b838`) | 9.339/9.847 | 0.549/0.556 | 0.346/0.370 | 16.66 |
| off (`00c44c`) | 9.232/9.741 | — | 0.195/0.211 | 15.81 |
| contact (`00cd0b`) | 9.312/9.896 | — | 0.410/0.436 | 15.94 |

The first pair ran with a wide p95; the second, steady pair puts contact
shadows at +0.08 ms opaque and +0.22 ms atmosphere, about 0.3 ms, within the
0.4 ms budget. The SDF row predates the GPU grid.

### SDF sun shadow measurement

M1 Pro, Release, dirty tree, local and not authoritative, 2026-10-17.
`tiled_bistro_plaza_timing_sun_sdf_replace_all` (the trace at every pixel,
2560×1440 at 0.6 with the spatial upscale, `local-offscreen-gpu-repeated`,
five repetitions of 300 frames), `Tiled.SunShadow` p50:

| Grid | Pass | Grid build alone |
|---|---|---|
| CPU, 128² over ±400 m (before) | 0.55 ms | — |
| GPU, 256², one-thread scan and one thread per caster | 1.25 ms | — |
| GPU, 512², the same | 2.83 ms | — |
| GPU, 512², SIMD-group scan | 2.07 ms | 1.60 ms |
| GPU, 512², block scan, a SIMD group per caster | 0.82 ms | — |
| GPU, 256², block scan, a SIMD group per caster (shipped) | 0.79 ms | 0.32 ms |

A single thread walking a large caster's cells at a low sun, and a scan
held to one threadgroup, dominated the build. With GPU-generated volumes the
shipped grid's capture of the plaza matches the earlier GPU grid's bit for
bit (`sha256:4cfbba06d76a8c51…`, `tiled_bistro_plaza_sun_sdf_replace_all`).

Authoritative windowed timing of the GPU grid at `cecf22e7`, clean worktree,
2026-10-17: `tiled_bistro_plaza_windowed_sun_{off,contact,epic,sdf_replace_all}`
under `performance-windowed-gpu.json` (hidden window, 1280×720 at 0.6 with
MetalFX, five repetitions of 300 frames), interleaved off, contact, Epic,
replace-all, off, contact, Epic; every report passed and was authoritative.
p50 in ms:

| Run | `Tiled.Opaque` | `Tiled.SunShadow` | `Tiled.Atmosphere` | Frame wall |
|---|---|---|---|---|
| off (`00f763`, `0109c0`) | 9.219, 9.213 | — | 0.191, 0.189 | 15.58, 15.56 |
| contact (`00f8cc`, `010861`) | 9.293, 9.296 | — | 0.406, 0.407 | 16.08, 16.14 |
| Epic, contact and SDF (`00fdd6`, `01145b`) | 9.300, 9.288 | 0.331, 0.327 | 0.431, 0.434 | 16.53, 16.62 |
| SDF everywhere (`010587`) | 9.290 | 0.789 | 0.338 | 16.93 |

Contact shadows cost 0.30 ms of GPU time and the distance field shadows on
top 0.36 ms, nearly all the grid build, since the plaza has few pixels past
the near cascades. On the Level Design Test's view across the valley
(headless editor at 1146×595, Epic, fixed scale), alternating the setting
three times gave 13.8 ms frames off and 14.45 ms on.

A pixel-footprint hit test in the trace (PETAFAB's pixel-cone epsilon: a miss
nearer than half the receiver's trace pixel ends the ray, and the smallest
step grows to match) left `Tiled.SunShadow` at 0.324 ms p50 against 0.324 and
0.324 ms without it (`tiled_bistro_plaza_windowed_sun_epic`,
`local-windowed-gpu`, M1 Pro, 2026-10-29, local), so it was not kept; work
on this pass starts at the grid build.

Cooked against GPU-generated volumes, the same binary's replace-all plaza
capture changes 20,583 of 921,600 pixels by more than 2 of 255, along
silhouettes such as bollards, window frames and the awning, where the cooked
volumes' unquantized positions and per-mesh scale shift self-shadowing; the
ground's shadows match. Metal API validation of
`tiled_bistro_plaza_sun_epic_validation` (contact and distance field
shadows, cooked volumes uploaded at creation) reported nothing.

On the Level Design Test (headless editor, Epic, view across the valley
from (300, 60, 450)), a diagnostic build that dropped the trace from the
atmosphere draw lost the viaduct's, its piers' and the tunnel portal's far
shadows once the far level rebuilt, while the terrain's and the forest's
remained: the far level leaves the field casters out, and the trace alone
draws their shadows. Before the field revision existed, the far level drawn
at load kept those casters indefinitely. Not done: the Level Design Test's
per-pass timing at a gate resolution, which needs a harness case of the
level.

### Sky edge measurement

Release, M1 Pro, 2026-10-07, on the lightmap-baked Bistro street view
(`tiled_bistro_baked_capture`, `local-offscreen`, 1280×720). Before the
split, pixels where a roof met a cloud took the clear sky's colour: one at
the roof's edge read (39, 75, 108) between the cloud's (109, 124, 137) and
the roof's (10, 20, 27), a dark blue outline along silhouettes against
clouds. After it the pixel reads (95, 111, 124); 299 pixels change by more
than 2 of 255, all along silhouettes against the sky
(`sha256:4ad06ae73a5f86ef00a56504a4a3142f74cb27ff533661f468d674ec187f89ef`,
`sha256:20d17b6103972fd679a8b11a2bc953b6a24446c4100eddac523d6ebc998d66d0`).
The atmosphere draw on the native static street view
(`tiled_bistro_street_static_native`, `local-windowed-gpu-single`, local,
300 frames) took 0.39 ms median before and 0.53 ms after; a diagnostic
build that read the alpha through programmable blending without the split
took 0.42 to 0.44 ms, so the split's code costs about 0.1 ms and
programmable blending about 0.04 ms
(`sha256:e377b76351f654151a44387854cce5affa55beaea1a6fa976f61affabe97119b`,
`sha256:63ec11919b4a2a134b503098181152b2d186a6c6097b4b01fee20eb06bc0e95e`,
`sha256:337e5c3322768709cb7a33ad112b383cf6b2261763542b05efe1929386065653`).

`HZB.BuildBase` builds the next frame's occlusion pyramid from the same
nearest-sample depth, so an object seen only through partly covered pixels
can be culled and stays culled. Treating partly-sky pixels as far was
rejected: once such an object draws, those pixels hold no sky, so it is
culled again the next frame and flickers. A conservative pyramid needs each
edge pixel's farthest sample, which costs the opaque pass another
multisampled target.

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
- **A depth pre-pass before forward shading.** Removed 2026-10-07: with
  opaque variants that never discard it only drew the opaque geometry twice
  ([measurement](#depth-pre-pass-measurement)).
- **Ordering alpha-tested draws near to far, laying their depth before
  their shading, or half-precision vertex outputs.** Not kept 2026-10-07:
  none lowers Bistro's cost ([measurement](#base-pass-measurement)).
- **Fixed upscaling on M-series.** Rejected by the owner on 2026-10-06:
  native resolution was the target, and decision 12 lowered it only while
  frames missed the budget. Revised by the owner on 2026-10-16: MetalFX
  temporal upscaling from 0.6 is the default
  ([ADR-121](121-tiled-temporal-upscaling.md)).

## Revisit when

- The tiled-pipeline prototype cannot reach the budget with the planned
  techniques.
- A desktop budget case misses the budget again, or lamps that must stay
  runtime lights, because they move or switch, take a Bistro case over it,
  and the fixed-resolution [levers](#desktop-budget-levers) do not bring it
  back; dynamic resolution then reopens
  [ADR-052](052-vulkan-fsr31-upscaling.md).
- A budget line goes over its p95 target, or a feature asks for the reserve.
- The 120 Hz line's `frame.wall` gap closes, or the owner wants a 120 Hz
  line at 2560×1440, which needs a Vulkan render scale with FSR upscaling.
- Apple9 dynamic register allocation changes the measured cost structure on M3
  or M4 ([ADR-083](083-supported-hardware-matrix.md)).
- An opaque fragment variant must discard or write depth: hidden-surface
  removal then loses its fast path, and a depth pre-pass may pay again.
- A mobile target is scheduled and needs its own budget, or a backend must
  run a second class (a tile-based Vulkan GPU, or an immediate-mode GPU on
  Metal).
