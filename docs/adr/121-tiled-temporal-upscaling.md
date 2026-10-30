---
status: partial
updated: 2026-10-30
authority: adr
---

# ADR-121: Tiled temporal upscaling

## Status

Accepted (partial). By default the tiled pipeline renders its Scene at 0.6 of
each axis with one sample per pixel, and the MetalFX temporal scaler rebuilds
it at the Scene output extent. Adaptive quality lowers the scale to 0.5. The
spatial upscale with 4× MSAA stays as a setting and as the fallback on a
device without Metal 4 MetalFX. The gaps are listed in
[Tiled temporal upscaling](../proposals/tiled-temporal-upscaling.md): there is
no reactive mask for blended surfaces, Custom material graphs take no mip
bias, the HZB history is lost under jitter, and there is no timing on a base
M1. All evidence is from the M1 Pro; see [Evidence](#evidence).

## Context

The tiled pipeline shimmered in motion on Bistro: string-light bulbs and
wires, ivy, lamp posts, window frames and cobbles. Its 4× MSAA samples the
same positions every frame and shades once per pixel, and the specular AA's
normal-variance scale was already at its best value. Its adaptive quality
upscaled spatially from as low as 0.65 of each axis
([ADR-039](039-metal-internal-render-scale.md),
[ADR-087](087-gpu-class-graphics-pipelines.md) decision 12).

Measurements on 2026-10-15 and 2026-10-16 rejected MetalFX at native scale as
an anti-aliasing pass: it cost about 5.3 ms. They selected MetalFX upscaling
with object and skinned motion, a negative mip bias and one sample per pixel.
The scaler costs about 4 ms more than the spatial upscale at the same internal
scale. At 0.5 and 0.6 its p95 is lower than spatial at 0.67, with less than
half the strong shimmer and more still detail. The rounds are under
[Evidence](#evidence).

Owner decision, 2026-10-16: ship MetalFX as the tiled default at internal
scale 0.6 with 1× MSAA, an adaptive floor of 0.5, and spatial with 4× MSAA as
the fallback. This keeps a long temporal history on the tiled pipeline, an
exception to the short-history rule
([Short-history rendering](../proposals/short-history-rendering.md#owner-decisions-2026-10-12))
like clouds and froxel fog.

## Decision

1. **Setting.** The graphics setting `upscaler` (`metalfx`, the default, or
   `spatial`; [`vkr_graphics_settings.c`](../../runtime/src/vkr_graphics_settings.c))
   exists only on the tiled pipeline and applies after a restart, because
   the multisample count is fixed when the renderer is created. It reaches
   the renderer as `VkrRendererBackendConfig::tiled_upscaler`
   ([`vkr_renderer.h`](../../renderer/src/vkr_renderer.h)).
   `vkr_renderer_tiled_upscaler` reports the upscaler in use. Metal's default
   render scale is 0.6, the cap of the adaptive-quality controller. Its floor
   is 0.5 under MetalFX and 0.65 under spatial
   ([`vkr_sample_runtime_config.c`](../../runtime/src/vkr_sample_runtime_config.c)).
   A harness case selects the upscaler with `renderer.upscaler`. The case
   default is `spatial`, so existing tiled cases keep their meaning, and
   `metalfx` enters the workload fingerprint.
2. **Fallback.** If `MTLFXTemporalScalerDescriptor supportsMetal4FX:` rejects
   the device, the Metal renderer logs a warning and creates the spatial
   path with 4× MSAA
   ([`vkr_metal_packet_setup.inc`](../../renderer/src/metal/internal/vkr_metal_packet_setup.inc)).
   `vkr_metal_packet_renderer_tiled_temporal` reports the result.
3. **Frame eligibility.** A frame takes the temporal path when it is tiled,
   uses a perspective projection and draws the default render mode to the
   default output (`vkr_renderer_prepare_tiled_temporal` in
   [`vkr_renderer.c`](../../renderer/src/vkr_renderer.c)). Other frames use
   the spatial tonemap. With one sample, they show unfiltered edges.
4. **Jitter.** [`vkr_temporal.c`](../../renderer/src/vkr_temporal.c) has one
   Halton (2, 3) generator. `VkrTemporalFrameInput::halton_phase_count`
   selects 8 phases for MetalFX and 16 for FSR 3.1 on the desktop pipeline;
   zero keeps SMAA's subsamples. Phases start at index 1, in internal pixels
   with y down. The phase advances by one per committed frame, not by frame
   number, so frame numbers with gaps still walk every phase in order. A
   120 Hz display at 60 frames a second would otherwise use only the four
   phases with x ≥ 0.
5. **Multisampling.** Under MetalFX the opaque pass draws one sample per
   pixel, straight into `hdr_scene_color` and the resolved depth. The
   multisample resolve would blur what MetalFX reconstructs.
6. **Mip bias.** The frame root's `texture_lod_bias` (byte 444, formerly
   reserved) is log2 of the render scale under MetalFX below unit scale, and
   zero otherwise. The tiled shading's material samples and terrain gradients
   apply it
   ([`tiled.metal`](../../renderer/src/shaders/metal/msl/world/tiled.metal)).
7. **Motion.** `Tiled.Motion` reprojects the resolved depth from the jittered
   current clip space into the previous unjittered one with eye-relative
   matrices (`vkr_temporal_reprojection`) and writes `tiled_motion` (RG16F,
   internal extent). If the frame has dynamic draws, two more passes run:
   - `Tiled.ObjectMotion` redraws them over that motion. It uses a motion
     culling view appended after every other view, which copies the camera's
     culling and levels of detail with `VKR_GPU_LOD_VIEW_DYNAMIC_CASTERS_ONLY`.
     It tests the resolved depth with a bias toward the camera.
   - After the scaler, `Temporal.TransformHistory` writes the frame's dynamic
     transforms by temporal index, stamped with the frame index, into
     `temporal_transform_history`.

   A skinned draw takes its previous vertices from the previous
   `skinning_output` history instance. The Metal packing sets
   `VKR_INSTANCE_TEMPORAL_OWNER` on the first row of each temporal index, as
   Vulkan does.
8. **Scaler.** `Tiled.Temporal` encodes one `MTL4FXTemporalScaler` from the
   display-linear colour (decision 11), resolved depth and motion into
   `tiled_temporal_color` (RGBA16F, Scene output extent). The scaler sets
   reversed depth ([ADR-109](109-reversed-camera-depth.md)), the jitter and
   a motion scale of the content extent. Its colour is already exposed, so
   it runs without auto exposure at unit pre-exposure. It sets
   `reset` when the frame has no valid history. Scale steps and gaps in
   frame numbers do not reset it: the previous camera, transforms and
   skinned vertices are the last committed frame's either way. The windowed
   editor skips about one frame number in three. Resetting on those gaps
   showed each such frame raw and jittered, which looked like heavy jitter
   on every edge of a still camera. `Tiled.ObjectMotion` and
   `Tiled.Stabilize` take the last submitted frame's history across a gap
   too.
   The scaler signals a fence, and the next encoder waits for it.
9. **Adaptive quality.** With dynamic resolution on, the scaler is created
   once for the `viewport_capacity` extent, the Scene output times the
   controller's maximum scale, with `inputContentPropertiesEnabled`. Each frame
   sets the input content to the viewport. Below the cap,
   `Tiled.TemporalStage` copies colour, depth and motion into the top-left
   corner of the capacity-sized images. A step therefore keeps the scaler and
   its history. Only a change of capacity or output recreates the scaler,
   after waiting for submitted frames.
10. **Still camera.** When the unjittered view-projection equals the previous
    frame's, `Tiled.Stabilize` keeps a running mean of MetalFX's output in
    `tiled_stable_color` (HISTORY) over up to 32 frames. The mean is clamped
    to the range of the current output's 3×3 neighbourhood. A pixel restarts
    its mean where motion is not zero or the clamp moves the mean by more
    than 5% of its luminance. A test of the pixel's own luminance change
    could not be used: jitter flicker changes a pixel by 10 to 40%.
    Post passes read that image on still frames and `tiled_temporal_color`
    on moving frames.
11. **Display-linear input.** Exposure and bloom read the internal
    `hdr_scene_color` after `Tiled.Blend`. `Post.DisplayLinear.Internal`
    then applies bloom, exposure, grading and the tone curve once per
    internal pixel into `post_display_linear_internal`, the transform of
    ADR-043's display-linear preparation, and MetalFX upscales that image.
    The final draw (`Post.Tonemap.Fullscreen`, or `Editor.Resolve` in the
    editor) reads the upscaled image, or its stable mean, as its
    display-linear source: it sharpens and applies the physical output
    scale only. Its frames take no output-size display-linear target
    (`post_transform_cache_enabled` is false;
    `tiled_temporal_display_linear` in
    [vkr_rg_json.c](../../renderer/src/vkr_rg_json.c)).
    `VKR_TILED_DISPLAY_LINEAR_UPSCALE_DISABLED` gives MetalFX the HDR colour
    and runs the transform after the upscale again, for diagnosis; the
    harness adds it to the workload fingerprint.
12. **Synchronization.** The cross-frame reads mark the previous instances
    used until this frame's submission completes, instead of placing
    write-after-read barriers. `Tiled.ObjectMotion` and `Tiled.Stabilize`
    place a queue barrier only while the previous frame's history may still
    be in flight.

## Consequences

- The tiled budget (ADR-087 decision 4) is now measured at the shipped
  configuration, MetalFX from 0.6 with adaptive quality down to 0.5, not at
  native scale with no upscaler.
- The tiled pipeline now has temporal history and motion vectors. Every
  effect added to it must declare how it handles motion: write motion
  vectors, write a future reactive mask, or draw after the upscale.
- Blended surfaces, glass included, enter the colour input without their own
  depth or motion. They reproject with the opaque surface behind them.
- MetalFX records no pass timestamps, so a per-pass profile does not show
  its cost. Its cost appears only in frame time.
- Frames outside the eligibility rules (orthographic, debug render modes,
  offscreen views) show one-sample edges without anti-aliasing under
  MetalFX.
- The HZB history requires an unchanged raster matrix, so jitter disables it.
  This costs only with a still camera.
- MetalFX and the stable mean filter display-referred colour, so an edge
  between a bright light and a dark surface blends as the display shows it
  instead of stepping. MetalFX's output texture must allow shader writes,
  which leaves it without lossless compression; the final draw reads it
  about 0.09 ms slower than a rendered display-linear target.

## Alternatives considered

- **MetalFX at native scale as anti-aliasing.** Rejected: about 5.3 ms at
  2560×1440 on the M1 Pro.
- **2× or 4× MSAA under MetalFX.** Rejected: the resolve blurs the input. 2×
  cost about 1 ms more than 1× for lower detail.
- **A different specular AA scale.** Rejected: 0.25 measured best, and the
  remaining shimmer is geometric.
- **A custom temporal upscaler.** Not pursued: MetalFX met the gates.
  Revisit if its black-box history cannot handle a later effect.
- **Recreating the scaler on every scale step.** Rejected: each step waited
  on the GPU and reset history.
- **`Tiled.Stabilize` writing the display-linear transform of its mean.**
  Shipped on 2026-10-30 for still frames (1.85 to 1.69 ms of post passes)
  and replaced the same day by decision 11, which removes the transform at
  the output extent from moving frames too.

## Revisit when

- A base M1 measurement misses the budget.
- Particles, scrolling textures or other blended effects arrive. Each needs
  a reactive mask or must draw after the upscale.
- MetalFX gains timestamps or a changed cost structure on newer GPUs.

## Evidence

### Display-linear input

Release on `4f63b43f` plus the change, M1 Pro, 2026-10-30, Bistro street view
at 2560×1440, MetalFX 0.6, `local-offscreen-gpu-single`, p50 GPU ms of the
post passes, two runs each, the same binary with
`VKR_TILED_DISPLAY_LINEAR_UPSCALE_DISABLED` as the before:

| Case | Before | After |
|---|---|---|
| Still, sharpness 0.25 | `Tiled.Stabilize` 0.73 / 0.72, `Post.DisplayLinear.Fullscreen` 0.71, tonemap 0.43 / 0.42: 1.87 / 1.85 | `Post.DisplayLinear.Internal` 0.27, `Tiled.Stabilize` 0.72 / 0.74, tonemap 0.50 / 0.51: 1.50 / 1.52 |
| Orbit, sharpness 0.25 | 1.86 / 1.85 | 1.52 / 1.49 |
| Orbit, sharpness 0 | `Tiled.Stabilize` 0.71 / 0.73, tonemap 0.71: 1.41 / 1.44 | `Post.DisplayLinear.Internal` 0.27, `Tiled.Stabilize` 0.72, tonemap 0.27: 1.26 / 1.26 |

Against the shipped still frame of the alternative above (1.69 ms), the
still frame saves about 0.2 ms. 2 s Metal System Traces show MetalFX's
pre-processing encoder at 0.55 against 0.65 ms on the still camera and 0.63
against 0.74 ms on the orbit without auto exposure; its other two encoders
are unchanged (about 0.92 to 1.4 and 0.97 ms). Offscreen `frame.wall` p50:
13.17 / 13.26 against 13.66 / 13.36 ms still, 13.73 / 13.72 against 14.11 /
13.91 ms orbit; these runs are not authoritative (dirty tree, offscreen).

Captures of `final_color` (`local-offscreen`, still after 60 frames and
mid-orbit): after against before 45.3 dB still, 46.6 dB orbit and 46.6 dB in
the editor, on 1.7 to 2.5% of the pixels, at most 95 of 255, all on edges;
two runs of one path differ by 64 to 75 dB. The edges that differ lie
against bright lamps and lit walls, where the HDR input gave stepped,
partly unfiltered silhouettes and the display-linear input gives filtered
ones.

### Stabilize with the display-linear transform

Replaced by decision 11 (see Alternatives considered). Release on `ddc9817a`
plus the change, M1 Pro, 2026-10-30, per-pass GPU
timestamps (`local-offscreen-gpu-single`), Bistro street view at 2560×1440,
MetalFX 0.6, image sharpness 0.25 (the shipped default, which turns the
display-linear cache on), p50 in ms, two runs each:

| Camera | Build | `Tiled.Stabilize` | `Post.DisplayLinear.Fullscreen` | `Post.Tonemap.Fullscreen` | Sum |
|---|---|---|---|---|---|
| Still | before | 0.714 / 0.720 | 0.706 / 0.707 | 0.424 / 0.426 | 1.85 |
| Still | after | 1.171 / 1.174 | not run | 0.516 / 0.517 | 1.69 |
| Orbit | before | 0.714 / 0.727 | 0.709 / 0.710 | 0.421 / 0.422 | |
| Orbit | after | 1.172 / 1.177 | 0.710 / 0.710 | 0.423 / 0.424 | |

The orbit holds still frames and moving ones; its moving frames, which take
the display-linear pass, are unchanged. The still frame's tonemap reads the
uncompressed storage image, 0.09 ms slower. Captures of `final_color` after
32 still frames: after against before 64.8 and 65.0 dB, at most 14 of 255 on
about 240 pixels; before against before 66.2 dB, at most 10 of 255; after
against after 66.6 dB, at most 14 of 255, so the difference is MetalFX's
run-to-run spread. Sharing the `Tiled.Stabilize` 3×3 neighbourhood through
threadgroup memory was 0.06 ms slower (0.89 against 0.83 ms with the
exposure and bloom work in the same encoder) and was not kept.

### Frame-gap reset fix

Release editor on `dec2c7de` plus the fix, M1 Pro, 2026-10-16, windowed
(screen locked; the Scene's `final_color` through `view.capture`), Testbed
"Level Design Test" scene, MetalFX 0.6 with adaptive quality, 2061×1062
Scene. Each view holds the camera still for 16 captures; the column is the
percentage of pixels whose luma standard deviation over them exceeds 8 of
255, which includes the FPS counter and other UI text:

| View | MetalFX before | MetalFX after | Spatial 0.67 |
|---|---|---|---|
| Valley along the line | 2.73% | 0.22–0.24% | 0.24% |
| Valley north | 1.48% | 0.24–0.42% | 0.23% |
| Interior | 0.42% | 0.26–0.30% | 0.27% |

Before the fix, 655 of about 1,900 frames had only the frame-gap reason
and reset MetalFX. After it, a run of the same views reset only at scene
load and at the camera placements. The headless editor and the harness
render every frame number and never showed the defect.

### Production round

Release build of this change on `c9b2c53f`, M1 Pro, 2026-10-16, from a
worktree with the change uncommitted, on the local, non-authoritative
profiles of the selection rounds. The references were regenerated on this
build. `c9b2c53f` changed the tiled shadow filtering after round (d), so
compare rows only within this round.

Image round (`local-offscreen`, the cases and metrics of the selection
rounds):

| Configuration | Still RMSE | Still detail | Pan shimmer | Strong shimmer | Pan detail | Still flicker |
|---|---|---|---|---|---|---|
| MetalFX 0.6 (default) | 5.24 | 0.65 | 0.66 | 2.27% | 0.39 | 0.003% |
| MetalFX 0.6, stabilizer with a 10% luminance test | 5.24 | 0.65 | 0.66 | 2.27% | 0.39 | 0.366% |
| MetalFX 0.5 | 5.50 | 0.78 | 0.65 | 2.15% | 0.45 | – |
| MetalFX, adaptive quality held at 0.5 in 0.6 inputs | 5.50 | 0.78 | 0.65 | 2.13% | 0.45 | – |
| Spatial 0.67, 4× MSAA | 5.88 | 0.42 | 1.12 | 5.52% | 0.41 | 0.000% |

- The neighbourhood clamp (decision 10) removes the still-camera flicker.
  The first stabilizer, which restarted a pixel whose luminance changed by
  more than 10%, left it unchanged.
- Adaptive quality held at 0.5 inside 0.6 inputs matches a fixed 0.5 to
  within rounding, so `Tiled.TemporalStage` and the per-frame input content
  are exact.
- Against spatial 0.67, MetalFX 0.6 has lower still error, 55% more still
  detail, 41% less pan shimmer and 59% less strong shimmer. Its pan detail
  is 0.39 against the gate of 0.41 that spatial 0.67 sets: a small miss.
  MetalFX 0.5 passes it at 0.45.

Crowd fixture (the selection rounds' metrics, frames 8 to 15):

| Configuration | Still, moving RMSE | Still, static RMSE | Pan RMSE | Pan crowd RMSE | Pan shimmer |
|---|---|---|---|---|---|
| MetalFX 0.6 | 6.73 | 5.16 | 6.08 | 3.95 | 0.76 |
| MetalFX 0.5 | 7.50 | 5.63 | 6.43 | 4.22 | 0.75 |
| Spatial 0.67 | 6.49 | 5.68 | 5.74 | 4.19 | 1.15 |

MetalFX 0.6 has lower error than spatial 0.67 on static surfaces and in
the crowd's box while panning. On the moving mannequins with a still
camera, and over the whole panning frame, its error is 4 to 6% higher.

Timing: `tiled_bistro_baked_native`'s orbit at a 2560×1440 drawable
(`windowed_hidden`, immediate presentation) on
`local-windowed-gpu-submission-single`. Configurations were alternated,
300 frames per run, `gpu.submission` median and p95:

| Configuration | Median | p95 |
|---|---|---|
| Native 1.0, 4× MSAA | 17.33, 17.23 ms | 25.95, 25.23 ms |
| Spatial 0.67, 4× MSAA | 11.21, 11.23 ms | 16.95, 17.01 ms |
| MetalFX 0.6 | 11.70, 11.60 ms | 14.54, 14.88 ms |
| MetalFX 0.5 | 10.46, 11.13 ms | 13.41, 15.18 ms |
| MetalFX, adaptive quality 0.5 to 0.6, 16 ms target | 11.23, 11.24 ms | 14.91, 14.29 ms |

- MetalFX 0.6 is within the 16.7 ms p95 budget with 1.8 to 2.2 ms of
  margin. Its median is 0.4 to 0.5 ms above spatial 0.67's, and its p95 is
  2.1 to 2.5 ms lower.
- In round (d) MetalFX 0.6 took 12.59 and 12.61 ms median, against 10.93
  and 10.97 ms for spatial 0.67. Here the gap is 0.4 to 0.5 ms. Decision
  12's barrier removal and skipping object motion without dynamic draws
  account for about 1.2 ms of the difference.

The crowd fixture's orbit (`crowd_orbit`, 16 skinned mannequins of 125,476
vertices each) misses the budget in every configuration, so it measures
relative cost only. Spatial 0.67 took 26.41 and 26.40 ms median and 35.92
and 37.03 ms p95. MetalFX 0.6 took 27.75 and 26.86 ms median and 33.45 and
33.41 ms p95: its object motion redraws every skinned vertex.

Editor: a headless Bistro editor run (`--headless --exec`, Release) reported
`gfx.upscaler` `metalfx`, render scale 0.6 and a 765×420 internal extent.
`Tiled.Motion` and `Tiled.Stabilize` appeared in the per-pass GPU log.
Assigning `"spatial"` set `gfx.restart`; `"bogus"` was rejected.

Unavailable: authoritative timing. On the clean commit,
`performance-windowed-gpu` came back incomplete for
`tiled_bistro_baked_metalfx`
(`sha256:accae9a384b3084d0155f97bb029f34bdf9c1f8b2f8937601808a084253bc7ed`)
and for the spatial `tiled_bistro_baked_native`
(`sha256:f4d3e4c4b19b85ceb52a7898def57024656cdf0e9a8fe58b97443b14607aae26`)
alike. Both stopped on `determinism.work_volume_mismatch`: on measured frame
4, `draw.shadow.cascade3.indirect_commands` differed between repetitions
(790 or 2 against 0). The far cascade's one-tile-a-frame rebuild is scheduled
nondeterministically on either upscaler. Timing on a base M1 is also
unavailable (no device). Vulkan is unaffected: it builds the shared temporal
and graph sources, and no native Vulkan run was made.

#### Production round digests

| Run | Report |
|---|---|
| Reference still | `sha256:13c231acad7639c13ea1857d54d82f1e595219b5cc5cfdc4fc591f2536784ecb` |
| Reference pan | `sha256:471f9f2d69c9442009bc02b54518d4f18edc150501f00daf9f58fa7609522f17` |
| Crowd reference still | `sha256:4d85a787b53c37c60794924530ab8e3060b9855fed6bcd88b41d1d9d26cc0bb2` |
| Crowd reference pan | `sha256:6af218fca94b24da3b536ca946409a906450265dc2c15f31d2291e7219d364be` |
| MetalFX 0.6 still | `sha256:336724d48cbcb0a4d46258c491d9343d99b1cd4f1e1495ecbefd8f88fa305202` |
| MetalFX 0.6 pan | `sha256:e6681f3d730277aa5d3595a9e8629c82f3514e6e0c0cadf8528dd6541ea975c1` |
| MetalFX 0.6 still flicker | `sha256:17fcf3a84b01551e76b41b84dd608ca31e7603eb5db2f0486d4e24e9d97ba315` |
| MetalFX 0.6 still flicker, 10% test | `sha256:4e35b80a949a49af88a9390b2e1627c7446be05369fefcbbc5b5c555543f7726` |
| MetalFX 0.6 crowd still | `sha256:dd944568a671f265888f54e4633e22016917e2341fa92bce780a840e63f41395` |
| MetalFX 0.6 crowd pan | `sha256:8acd7fa5428ca1f246c5502c9a66c950169061323a782df26b2d3449c8801e43` |
| MetalFX 0.5 still | `sha256:29ce59fa2f3fcd764f2fc077a3b63fb26a836e3912b835958714082267ede188` |
| MetalFX 0.5 pan | `sha256:d54be23821f3190960643ce3c5b937221a2c0da6d5be166697802910abd8a568` |
| MetalFX 0.5 crowd still | `sha256:d519402dec1b1d66d23a188c57a26d19aeb2a117f259e3c52fdcb07fad406f06` |
| MetalFX 0.5 crowd pan | `sha256:2c6401d233222146fec66e7032a3f58d7d04ba0e2b53afe2648d41c5b0a03a4e` |
| Adaptive held at 0.5 still | `sha256:c73e096fe666772c429e234c2c93605fe9e1b4cfdc7144030c915bc4e8f0671c` |
| Adaptive held at 0.5 pan | `sha256:ad48d5354f39dfa204c34bc7be7b581803b3d0b9df21d8c8a0b645958393e467` |
| Spatial 0.67 still | `sha256:d6d297c0006dca6ae4d9ec3d9089db70cdf774895e7463c3caccd594a077d677` |
| Spatial 0.67 pan | `sha256:a060d2677efe4dcce3d49a313cbbea285528f06ee9afd1881917f74c26ba33b0` |
| Spatial 0.67 still flicker | `sha256:646ef6027004b29db5c20ae69eb2f8802d8486d9684d6bdf0834180caa250b49` |
| Spatial 0.67 crowd still | `sha256:228e2a11dea95aa1c30d586eb1971dc8db973f83f81b05a90ed02ca88030a494` |
| Spatial 0.67 crowd pan | `sha256:a16e3134f9249267bbc385c5771d2e9798d768295de6223b7cbc3b357e3f6363` |
| Native 1.0 timing, runs 1 and 2 | `sha256:c293ead848145785ac3b454a14af1f245b0f41cbc2b0b8b4720a04c2de208ff9`, `sha256:b6edf5817a0cb7b1bce3faaf11533cb67c4f973cf0e73636b9d182bb2e88d38a` |
| Spatial 0.67 timing | `sha256:5cda02413bf4524f99c11a07b079be4696dc0e331f395757c844796634663767`, `sha256:a36b57b4d7656472b220ef30899023b30bad075a0afee2aeefcc44c9e92032af` |
| MetalFX 0.6 timing | `sha256:8ce12a7d4740f01087c58e055c532e35730eb514d231edcc75cba4e1f61347b4`, `sha256:a9f668ad95d3612eb92e41d9dadc06ac968488ef4e6aa2d62dd037f5ca8eab84` |
| MetalFX 0.5 timing | `sha256:672981be61943ef6e49002e386d3435645458bb3ef3911709bf411bf0ecfc1ec`, `sha256:b0f15fa2348650ee0074296f137b1eb5654feb09603cad5ecfcfc32102975f7c` |
| MetalFX adaptive timing | `sha256:51740d79616a3a2de6b30ab025f31a090ba4c09ed102e4baf3ea65f45facb980`, `sha256:a036304aacf4ea0db2cdd3ff4017f19eef6739ff370969a57c5bbdbcce821246` |
| Crowd orbit, spatial 0.67 | `sha256:afc26c86d84aeef0ffad26d4c22d5c6fd84573a3284036c05acd97a1a0cd8cf6`, `sha256:8d18c97693dd762f3e6d8321a84d9401103e10733fde4a8d26d9e2b93e371bb8` |
| Crowd orbit, MetalFX 0.6 | `sha256:312d021572cedb94ddf64727219d6aadf61be229eba4cae7a5f01106c2a609e6`, `sha256:3ea244b8938e53752ae7c7795eb5832c1ada71b4129479217d21b6b7696ab26e` |

### Selection rounds

These rounds chose the configuration, using a prototype controlled by
environment variables that production replaced with the `upscaler` setting.

All runs: Release, M1 Pro, 2026-10-15 (rounds a to c) and 2026-10-16
(round d). They used local, non-authoritative
profiles and are not baselines. Each build came from a detached worktree with
uncommitted changes (a dirty tree): the spec-AA round from `5e5d0331` with
edits to the normal-variance scale, the MetalFX rounds from `553b970d` with the
prototype patch. The exact command lines were not kept. Case names and
profiles are below; report digests are in [Selection round digests](#selection-round-digests).

These local runs measured native scale at 23.4 to 24.5 ms `gpu.submission`
p95 on `tiled_bistro_baked_native`. ADR-087 records 15.5 ms p95 at native
scale for the lightmap-baked Bistro. Compare rows only within one round; the
absolute times do not replace ADR-087's numbers.

#### Image metrics

The image rounds capture `bistro_tiled_local` from the
`specular_aa_bistro_static` camera with manual exposure and bloom off, on the
`local-offscreen` profile. Round (b) used exposure 0.853, and round (c)
reused its setup. Each test capture is 1280×720. The reference is the same view at 2560×1440 with 4× MSAA,
box-downsampled by 2 in linear light. Every metric works on Rec. 709 luma of
sRGB-encoded values on a 0 to 255 scale. The camera pan has 8 frames.

| Metric | What it measures |
|---|---|
| Still RMSE | Root-mean-square luma difference between the still capture and the reference. Lower is closer. |
| Still detail | High-frequency energy of the still capture divided by the reference's. High-frequency energy is the mean square of luma minus its 3×3 box mean. 1.0 matches the reference; above 1.0 is excess edge and sparkle energy (aliasing); below 1.0 is blur. |
| Still excess HF | Round (b) only: the mean, over pixels, of the positive part of the capture's squared high-pass minus the reference's. It counts only energy the reference does not have. |
| Pan shimmer | For each pixel, the mean of `abs(f[t+1] - 2 f[t] + f[t-1])` over the pan frames, averaged over the image. A steady pan changes a pixel at a near-constant rate, so the second difference shows frame-to-frame flicker. Real content change also scores: the round (b) reference pan scores 1.11. |
| Strong shimmer | The percentage of pixels whose pan shimmer exceeds 4. |
| Pan detail | Still detail measured over the pan frames against the reference pan. It shows how much detail survives in motion. |
| Still flicker | Camera fixed for 8 frames: the percentage of pixels whose luma standard deviation exceeds 2. It shows jitter that the temporal filter does not settle. |

Read shimmer together with detail. Blur lowers shimmer too: spatial 0.5 has
shimmer 1.15 at a pan detail of 0.21.

#### (a) Specular AA tuning

The first question was whether a different normal-variance scale removes the
shimmer without temporal work. The round compared scales on the `spec` view
above and on the `tiled_bistro_baked_capture` camera (`street`), with a yaw
pan of about 1/3 pixel per frame. Masked RMSE is still RMSE on the pixels
where the scale variants differ by more than 1. Pan error flicker is the
per-pixel standard deviation of (capture minus same-pose reference) over the
pan frames, on the same pixels.

| Normal-variance scale | Masked RMSE, `spec` | Masked RMSE, `street` | Pan error flicker |
|---|---|---|---|
| 0 | 10.94 | 9.20 | 3.88 |
| 0.25 (current) | 10.71 | 8.91 | 3.67 |
| 1.0 | 11.14 | 9.50 | 3.70 |

A scale of 4.0, built in the shared tree and only indicative, scored worse
again (12.28 on `spec`). The current 0.25 is best on all three metrics, and
the code did not change. The remaining shimmer is geometric (bulbs, wires,
ivy, lamp posts, window frames), which specular AA does not address.

#### (b) Native-scale MetalFX temporal AA

MetalFX ran at render scale 1.0 as a temporal anti-aliasing pass (input and
output extents equal). A is production, B adds MetalFX, C adds MetalFX with 2×
MSAA.

| Config | Still RMSE | Still excess HF | Still detail | Pan shimmer | Strong shimmer | Pan detail | Still flicker |
|---|---|---|---|---|---|---|---|
| A: 4× MSAA | 3.54 | 11.4 | 1.16 | 1.37 | 7.2% | 1.15 | 0.000% |
| B: MetalFX, 4× MSAA | 3.08 | 2.6 | 0.63 | 0.53 | 1.1% | 0.42 | 0.354% (max std 11) |
| C: MetalFX, 2× MSAA | 3.45 | 3.6 | 0.66 | 0.56 | 1.3% | 0.41 | 0.502% (max std 22) |
| Reference | – | – | 1.00 | 1.11 | 5.6% | 1.00 | – |

MetalFX cut pan shimmer by more than half and strong shimmer from 7.2% to
1.1%. The image became softer than the reference (detail 0.63), and a still
camera flickered on 0.35 to 0.50% of pixels.

Two checks confirmed the inputs. Flipping the jitter sign in x, y or both
raised still RMSE to 3.25 to 3.96, so the signs are correct. The horizontal
shift that best matches each pan frame to the reference was 0.00 pixels on
every frame (A alternates ±0.1), so the camera motion vectors are aligned.

Timing: `tiled_bistro_baked_native` (orbit, 2560×1440 drawable,
`windowed_hidden`, immediate presentation), `local-windowed-gpu-submission-single`,
runs alternating A, B, C, A, B, C, 300 frames each.

| Config | `gpu.submission` median / p95 |
|---|---|
| A | 15.44 / 23.55, 15.51 / 23.43 ms |
| B | 20.96 / 28.99, 20.99 / 29.30 ms |
| C | 19.92 / 26.77, 19.92 / 27.33 ms |

With `local-windowed-gpu-single`, `Tiled.Motion` took 0.13 ms and
`Tiled.Opaque` took 10.82 ms (B) against 10.85 ms (A). MetalFX records no pass
timestamps, so its cost of about 5.3 ms comes from the frame difference. B
adds 5.5 ms median and C 4.4 ms. Native-scale temporal AA does not fit the
budget and was rejected.

#### (c) Temporal upscaling against spatial and native

The second round rendered the scene at a lower internal scale and let MetalFX
upscale it to the output. N is production at native scale; S is production's
spatial upscale at a fixed scale; T is MetalFX with 4× MSAA; T2 is MetalFX
with 2× MSAA. All rows use image sharpness 0.25
([ADR-043](043-presentation-dpi-and-color-transfer.md)) except where
noted.

| Config | Scale | Still RMSE | Still detail | Pan shimmer | Strong shimmer | Pan detail | Still flicker |
|---|---|---|---|---|---|---|---|
| N | 1.0 | 3.87 | 1.32 | 1.49 | 7.97% | 1.31 | – |
| S | 0.67 | 5.88 | 0.42 | 1.12 | 5.51% | 0.41 | – |
| S | 0.5 | 7.18 | 0.21 | 1.15 | 6.11% | 0.21 | – |
| T | 0.67 | 4.34 | 0.45 | 0.50 | 1.05% | 0.31 | 0.23% (sharpness 0) |
| T | 0.5 | 5.56 | 0.39 | 0.47 | 0.92% | 0.26 | not measured |
| T2 | 0.5 | 5.74 | 0.51 | 0.49 | 1.13% | 0.33 | 0.359% |

The motion vectors were aligned at 0.67 (best-match shift 0.00 pixels on every
frame).

Timing: the same case, profile and alternation as round (b), sharpness 0.25,
300 frames per run.

| Config | Scale | `gpu.submission` median / p95 |
|---|---|---|
| N | 1.0 | 16.23 / 24.46, 16.00 / 24.32 ms |
| S | 0.67 | 10.68 / 16.34, 10.76 / 16.68, 10.61 / 16.52, 10.82 / 17.26 ms |
| S | 0.5 | 8.75 / 14.22, 8.88 / 13.87 ms |
| T | 0.67 | 15.03 / 20.03, 14.88 / 20.17 ms |
| T2 | 0.67 | 13.45 / 19.61, 13.34 / 19.27 ms |
| T | 0.5 | 12.89 / 17.51, 12.79 / 17.57 ms |
| T2 | 0.5 | 11.18 / 15.48, 11.14 / 15.51, 11.11 / 15.81 ms |

Results:

- T2 at 0.5 is the only tested MetalFX configuration with p95 under 16.7 ms.
- Against S at 0.67, which is near today's adaptive floor of 0.65, T2 at 0.5
  has about 1 ms lower p95 and 0.4 ms higher median. Its still RMSE is
  slightly lower (5.74 against 5.88), its still detail is higher (0.51 against
  0.42), and it has about 5× less strong shimmer (1.13% against 5.51%).
- In motion T2 at 0.5 keeps less detail than S at 0.67 (pan detail 0.33
  against 0.41). The prototype has camera motion only, and texture sampling
  has no negative mip bias.
- MetalFX upscaling to 1440p costs about 4 ms over the spatial upscale at the
  same internal scale (T against S at 0.5 and at 0.67).
- 2× MSAA saves about 1.5 to 1.7 ms median against 4× under MetalFX at both
  scales. At 0.5 the image metrics change little: still RMSE 5.74 against
  5.56, strong shimmer 1.13% against 0.92%.
- Visible defects in the 0.5 crops: cobbles smear, which suggests a missing
  negative mip bias, and a fine crosshatch appears on one facade in the still
  crop. The crosshatch is not explained.

#### (d) Object motion, mip bias and 1× MSAA

Same machine and profiles as (c), on 2026-10-16. The prototype gained the
first three planned steps:
per-object and skinned motion vectors, a negative texture mip bias of
log2(render scale), and a 1× MSAA option. All rows below use image sharpness
0.25, the game runtime's default.

The Bistro crowd fixture
([`bistro_tiled_crowd_local.scene.json`](../../assets/scenes/fixtures/bistro_tiled_crowd_local.scene.json))
holds 16 animated mannequins on the street of the image round's camera:
five walkers crossing the view, which the harness moves at 1.90 m/s, and
eleven that run, jog, jump, crouch or idle in place, three of them
overlapping. Sixteen is the most the skinned-vertex capacity takes: each
mannequin has 125,476 vertices and the capacity is 2,097,152. The harness
runs no gameplay scripts, so authored movers do not move. The crowd metrics
compare 8 frames, measured frames 8 to 15, with the 2× reference: error on
the pixels the reference changes over those frames (the moving
mannequins, 1.98% of the frame) with a still camera, error elsewhere, and
whole-frame error, error in the crowd's screen box and shimmer while the
camera pans against the walkers.

| Configuration | Still, moving RMSE | Still, static RMSE | Pan RMSE | Pan crowd RMSE | Pan shimmer |
|---|---|---|---|---|---|
| N 1.0 | 3.58 | 3.92 | 3.86 | 2.59 | 1.53 |
| S 0.67 | 6.48 | 5.67 | 5.73 | 4.19 | 1.15 |
| MetalFX 0.5, 2×, camera motion only, no mip bias | 9.46 | 6.20 | 6.54 | 5.21 | 0.62 |
| MetalFX 0.5, 2×, camera motion only, mip bias | 8.76 | 5.90 | 6.30 | 4.59 | 0.66 |
| MetalFX 0.5, 2×, object motion, mip bias | 7.32 | 5.89 | 6.27 | 4.24 | 0.69 |
| MetalFX 0.5, 1×, object motion, mip bias | 7.28 | 5.55 | 6.26 | 4.13 | 0.75 |
| MetalFX 0.6, 1×, object motion, mip bias | 6.54 | 5.03 | 5.90 | 3.89 | 0.75 |

Object motion removes the ghosted limbs camera-only motion leaves on the
walkers and on mannequins animating in place; the mip bias lowers error on
static surfaces. On the image round's Bistro view and pan:

| Configuration | Still RMSE | Still detail | Pan shimmer | Strong shimmer | Pan detail |
|---|---|---|---|---|---|
| MetalFX 0.5, 2×, no mip bias (round c) | 5.74 | 0.51 | 0.49 | 1.13% | 0.33 |
| MetalFX 0.5, 2×, mip bias | 5.39 | 0.53 | 0.55 | 1.16% | 0.34 |
| MetalFX 0.5, 1×, mip bias | 5.42 | 0.88 | 0.64 | 2.05% | 0.52 |
| MetalFX 0.6, 1×, mip bias | 5.10 | 0.71 | 0.63 | 2.01% | 0.44 |

With one sample the multisample resolve no longer blurs the input MetalFX
reconstructs, so the image is sharper and shimmers somewhat more. Timing on
`tiled_bistro_baked_native`, two alternated runs each:

| Configuration | `gpu.submission` median | p95 |
|---|---|---|
| N 1.0 | 16.16, 16.26 ms | 24.33, 24.43 ms |
| S 0.67 | 10.93, 10.97 ms | 16.87, 16.76 ms |
| MetalFX 0.5, 2× | 12.00, 12.03 ms | 16.97, 16.57 ms |
| MetalFX 0.5, 1× | 11.00, 11.00 ms | 15.13, 14.91 ms |
| MetalFX 0.6, 1× | 12.59, 12.61 ms | 15.53, 16.12 ms |

The object motion build costs MetalFX 0.5 with 2× about 0.9 ms median against
round (c), although the baked fixture draws no dynamic object. The likely
cause is the queue barriers that order the previous frame's transforms and
skinned vertices across command buffers; they are not yet optimized.

#### Selection round digests

Round (b) image captures, in order A, B, C, then the jitter-sign checks:

| Run | Report |
|---|---|
| A still | `sha256:7eba887de4f6fd7e3bb01bbccfcb3f89cb7f83f3b884b4127b32cb60eff90e75` |
| A still flicker | `sha256:7bca8f6139c8bc5b5f4ead9fdfd0c2e5a8ffd82e935ba8925371af110ebaa8e1` |
| A pan | `sha256:4b11d0025090e7eddb825ca1cf453937268ea0b5c3e3333afb72e300d184cc53` |
| Reference still | `sha256:199c2d7df62dfad62718617b7a644688af19fae253794f3833676f824c0cd8f3` |
| Reference pan | `sha256:059828115b941d23db2a1c1170a040bdec4ff5c5f91803fad95a043be0a93bd6` |
| B still | `sha256:a63c0a6de8a2480f8c930e6db51d216eb2ae20b13c4b0eab7f9812f55afe59a8` |
| B still flicker | `sha256:92044a5379eb0c2b0e0b394d438155ab203e47df3c4d7136fec90db01a1c42ce` |
| B pan | `sha256:dc18d64e1740785c46ec8cb0e02825df4763f5e87c2d7a0fea279255adce76ee` |
| C still | `sha256:899560ef84b5bb3b5f10df16cf2ecd4bd6031853e990df94c5dd71b5e5b0f766` |
| C still flicker | `sha256:ddaf60e556a3e6042bd4459519e3376f12e9b18d5102429a9e3a911fab8d5159` |
| C pan | `sha256:7d1a8d2ca8a2347f1074760225e9f523c01c5d898c453a96657ec838181251f7` |
| Jitter flipped x and y | `sha256:0f80fdf4cf0ace56c7812f44af5f62135f56ef9e61ec56c0108c98c6b93e7139` |
| Jitter flipped x | `sha256:afed6a6e282f943cf6683548d16eecd686752a6f70575d831a3aa69a8a6bcda3` |
| Jitter flipped y | `sha256:0c7b191763b888d5254dbaa498d73986f19e6949b308508430b3ef8ce9124688` |

Round (b) timing, in run order:

| Run | Report |
|---|---|
| A, run 1 | `sha256:7946324d00a53655f035b81e99abafda50388a4087a9a3bed5f9181fd4894108` |
| B, run 1 | `sha256:2634cd7f71608f069051de76fd99ff5b7eacd5517fb03c0c6a52104833395656` |
| C, run 1 | `sha256:dc4e553d7e4804f90ea4a2afcc6a142005c006fd3afeaf1a94c2536cba36a4ff` |
| A, run 2 | `sha256:82ca32b3b66c4d4b07ba0a9b244a10618de2487ad538b7b6d29ce6523b1037e6` |
| B, run 2 | `sha256:11e7fb6b185945fa9b31f47ceb353b40599650a578e748049f497a0fcabb6332` |
| C, run 2 | `sha256:6ccfced8e11ab9043b83f8950e5f309a1d48509ba5aa93bd34ca6bae3d4c4f94` |

Round (c) image captures at sharpness 0.25, and T's still-flicker capture at
sharpness 0:

| Run | Report |
|---|---|
| Reference still | `sha256:3041bd69ed48d77c72b184f6ddae16c2ac74f377072901d9e36cc9232793a60d` |
| Reference pan | `sha256:4a80d373312ad4f3c5848ec9154cd83b12066c9591ccf549b1f745c1075be1ec` |
| N 1.0 still | `sha256:33760216487c4794b976dfc1d3963b3625d9719f0fcd21a38123a23cad6579f3` |
| N 1.0 pan | `sha256:905aae7916228bb52b45e60d1a5cd07a14bb369bbb5219f47b39a5db4f08ca7c` |
| S 0.67 still | `sha256:a6d7a23e482bcf7de5fc884b184bf949f0222cea6a7640132bf2bd2b5550363e` |
| S 0.67 pan | `sha256:c564c7b5a5086565d4eac188ecce422693a9c1a8919dc7e44366d02fc2039ae2` |
| S 0.5 still | `sha256:5134320203c7a64aee958b1a5c7f120e9a274253013eb0222bda03377a1adcb8` |
| S 0.5 pan | `sha256:984eac48d4e3a2dfc635ab21d6e1aa8bc1e647e8ac756b2230f3a849aba58a67` |
| T 0.67 still | `sha256:88bccab9bfc2bd9e76b76bcf68c729d53a33b75b1594f6078b07dcddc56b4cd5` |
| T 0.67 pan | `sha256:de5b51541d2b1fa6e783dd6fa48f3573a25575fa1955b164d400e62b6731c6c5` |
| T 0.67 still flicker, sharpness 0 | `sha256:1874ea7e206f32094e6270f34dea0d2447b9edfb0964542aaa8e678a14bb4faf` |
| T 0.5 still | `sha256:b0ceec9126b2d9438785c4859497864d235c4c92cb70de17e08484b3a9f839a0` |
| T 0.5 pan | `sha256:25c97adf1f63982d0aaa4c5af28525319103ee7f170257c28d7b0613a48b618e` |
| T2 0.5 still | `sha256:ad49c7988b9aa29a85680aab74f46595d3c31da357853cc6575d77afadfedc05` |
| T2 0.5 still flicker | `sha256:c789bb74217aafcd2103b3e5d528229f07bc8269bf547852e73aed06443be4f3` |
| T2 0.5 pan | `sha256:4b0526f421981038ba2c4a5eface481a282626a30fef1e05e4992fe988de9c37` |

Round (c) timing, in run order:

| Run | Report |
|---|---|
| N 1.0, run 1 | `sha256:6b7a6605487bcb89a64086fcb7e5d8a031f53491275bb9d254a511cc32e2a017` |
| S 0.67, run 1 | `sha256:44db333de4520ed527c475160e597eb436cff3cded81fbea1f1dc47b6e3ddb1f` |
| T 0.67, run 1 | `sha256:82b4b8ad72e02c25246654e7fea7fa04988abeb5633d835c2636d88cbbce0636` |
| T2 0.67, run 1 | `sha256:77ba9948f4cb6beac040511eb9c279fd00cecf69af3ed3a687203f096dab76eb` |
| N 1.0, run 2 | `sha256:db3239b075c907544f5beff1209015f09b24b84cfec648e323028f63ecb95ee4` |
| S 0.67, run 2 | `sha256:a4920336329c3c17a4851fd12af0bb050fd960eaa828558bee49aa23425ebdd1` |
| T 0.67, run 2 | `sha256:9c480c73619a34b76fe23630bbfd4b8570cb883d753f471d3a98989f81bc6cf2` |
| T2 0.67, run 2 | `sha256:e701e06e4bc8d81353718bae3a09577c6e7eefcbaa2d6964b6ee942b25fb8af7` |
| S 0.5, run 1 | `sha256:e0bd84b2e5dd95d5f7eba4b983b91a8327b1d098361095c59be2d945e8da30f8` |
| T 0.5, run 1 | `sha256:5f68670602e7be6a956eea1aae3d9f623c084101c581cf04db19467b4fcb0cdb` |
| S 0.5, run 2 | `sha256:a402f6618fe5fe1fc914737b2daf92e88c1f9aabca30e318a43ebedfe4da1184` |
| T 0.5, run 2 | `sha256:60c8448a6f87de6b5362fd43206eb8efd26b8f1830ab4b323ef7ec2ad334fac7` |
| T2 0.5, run 1 | `sha256:481a9d63c64e445f772fac96c4dcbdbb1f4866bc027d645ff6dcc07f732dee4b` |
| S 0.67, run 3 | `sha256:7e2031b1562dabed0132c59633f1e65fc37511ce71c06314b7b37b5c06cb7021` |
| T2 0.5, run 2 | `sha256:5d8fd31158b01f2e32dce241ba82eea23836c41e69e8ab1e8c4c70c4940eaf2e` |
| S 0.67, run 4 | `sha256:073442ace6480116fddd781c1db3319d3e4ca722e75188421bc9ae715d11d45a` |
| T2 0.5, run 3 | `sha256:1cf4a90a82dca37a1e1c79fec57e7521fb15bdc62b7edb871095d1a97134a53c` |

The round (a) report digests were not kept.
