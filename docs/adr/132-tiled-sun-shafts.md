---
status: implemented
updated: 2026-10-22
authority: adr
---

# ADR-132: Screen-space sun shafts on the tiled pipeline

## Status

Accepted. Since 2026-10-22 the tiled pipeline draws screen-space sun shafts
where a scene's volumetric fog component is on. They are the tiled class's
realization of that component; the desktop pipeline keeps froxel fog
([ADR-059](059-froxel-volumetric-fog.md)). Native evidence is Metal only
(the class's one backend); no Metal validation run.

## Context

The tiled pipeline turns froxel fog off
([ADR-087](087-gpu-class-graphics-pipelines.md), decision 7): its injection,
integration and shadow sampling do not fit the M1 frame budget. Analytic
height fog ([ADR-057](057-analytic-height-fog.md)) still applies on the
tiled class, but it cannot show light shafts, so a scene with the
`volumetric_fog` component looked the same on the tiled class with the
component on or off.

Owner decision of 2026-10-22: froxel fog stays off on the tiled class.
Instead it draws screen-space sun shafts: a half-resolution pass that blurs
a depth-derived sky mask radially toward the sun's place on the screen
(GPU Gems 3, chapter 13; Half-Life 2: Episode Two), about 0.3 ms on an M1
Pro at the class's internal resolution. Shafts show only while the sun is in
or near the view and fade out otherwise; a sun far off the screen adds
nothing. No new component: the existing one carries the authored values.
ADR-087 decision 3 lets the classes differ in screen-space effects.

## Decision

1. **Authored contract.** The scene's `volumetric_fog` component reaches
   every frame as `VkrFrameInput.globals.froxel_fog`
   ([`vkr_froxel_fog.h`](../../renderer/src/vkr_froxel_fog.h)). On the tiled
   class `enabled` turns the shafts on, `color` tints the scattered light,
   `density` (per metre) sets their strength and `anisotropy` is the
   Henyey-Greenstein g of the phase between the view ray and the sun. The
   component's height falloff, base height, density boxes, sky lighting and
   local-light scattering have no tiled effect. Analytic fog is unchanged.
2. **When.** Frame preparation
   (`vkr_renderer_prepare_tiled_sun_shafts` in
   [`vkr_renderer.c`](../../renderer/src/vkr_renderer.c)) sets
   `VkrPreparedFrame.tiled_sun_shafts` for a tiled, perspective frame in the
   default render mode with scene rendering, the fog on with a positive
   density, and a shining sun. It also sets `tiled_sun_shafts_fade`, the
   product of two ramps: the sun's elevation sine over 0.05 (zero at the
   horizon, full above about 3°), and the sun's screen position, full inside
   the view and zero 0.5 NDC units past its edge (about 10° past the edge
   of a 70° view). A sun behind the camera, below the horizon or with zero
   fade leaves the flag off, and then the frame has no `tiled_sun_shafts`
   image and no `Tiled.SunShafts` dispatch (graph condition
   `tiled_sun_shafts`).
3. **Pass.** `Tiled.SunShafts` (`pass.tiled.sun_shafts`,
   [`tiled.rendergraph.json`](../../assets/render_graphs/tiled.rendergraph.json))
   is a compute pass after the opaque passes with two dispatches over half
   the internal viewport
   ([`sun_shafts.metal`](../../renderer/src/shaders/metal/msl/world/sun_shafts.metal)),
   one root (`VkrMetalTiledSunShaftsRoot`, 176 bytes) and a barrier between
   them. It reads `opaque_vbuffer_depth` and, with clouds, `cloud_shadow`,
   and writes two transient images: `tiled_sun_shafts_source` (R8) and
   `tiled_sun_shafts` (R16F).
   - The source: one gather reads each texel's 2×2 block of the resolved
     depth; its share of far-plane samples, reversed depth 0
     ([ADR-109](109-reversed-camera-depth.md)), is scaled by the square of
     the remaining fraction of 0.5 view heights between the texel and the
     sun's place on the screen (`sun_uv`, from the jittered projection the
     depth was drawn with). Light therefore streams from the sun rather than
     from the whole sky.
   - The march: each texel takes 24 bilinear samples of the source toward
     the sun, over at most 0.5 view heights, with weights that fall by 0.935
     per step from the texel, so an occluder's dark shaft fades away from it.
     Frames that MetalFX accumulates start the march at an offset that moves
     with the frame (`vkr_local_shadow_contact_noise`), and the upscaler
     averages it; other frames keep it fixed. The weighted mean is
     multiplied by the Henyey-Greenstein phase `vkr_fog_phase_hg(g, v·s)`
     for the texel's view ray v and the direction to the sun s, and by the
     cloud shadow over the camera (`vkr_metal_packet_sky_cloud_shadow`).
4. **Composite.** `Tiled.Atmosphere` takes a pipeline variant with function
   constant 4 (`vkr_tiled_sun_shafts`) in shaft frames, so frames without
   shafts run the draw unchanged. The variant samples the shafts bilinearly
   (binding 39) and adds, after every other medium of the pixel,
   `E · c · fade · S · (1 − exp(−density · min(d, L)))`: E the sun's
   pre-exposed light, c the fog colour, S the shafts' value and d the
   distance along the camera axis to the pixel's nearest surface, L for
   sky. L is the fog's `max_distance`, at most 10 m
   (`VKR_METAL_SUN_SHAFTS_PATH_M`). The scattered share therefore runs from
   about a tenth at density 0.01 through a fifth at 0.02 to two fifths at
   0.05: visible, not blown out, at Bistro's sun (intensity 1.9) with
   automatic exposure. A pixel mixing sky and surface samples weights the
   two parts by the surfaces' resolve share, as the other media do. The sky
   root grows from 224 to 256 bytes (`sun_shafts`, `sun_shafts_density`,
   `sun_shafts_sky_share`, and `sun_shafts_tint` with E · c · fade in rgb
   and L in w).
5. **Editor.** The Pass output view's `sun-shafts` source shows
   `tiled_sun_shafts` on the tiled class.

## Consequences

- Frames without the fog, or with the sun out of reach, are unchanged:
  the graph drops the pass and both images, and the atmosphere draw keeps
  its variant without shafts. A Bistro capture with the component off
  matched the image before the change bit for bit.
- The shafts are screen-space: occluders outside the view cast none, the
  far plane is the only light source, clouds count as sky (their shadow
  over the camera dims E), and surfaces take scattered light along their
  view ray whether or not the air in front of them is in shadow.
- With the sun just outside the view, marches toward it read the edge of
  the depth image.
- `Tiled.Atmosphere` runs whenever the shafts do, even without clouds or
  fog.

## Alternatives considered

- **Froxel fog on the tiled class.** Rejected by the owner: its volume,
  injection and history cost more than the class's budget allows.
- **Radial blur of the lit image** (GPU Gems' occlusion pre-pass over
  colour). It needs a bright-pass copy and smears bright surfaces; the depth
  mask needs only the resolved depth, which the class already keeps.
- **A full-resolution march.** Four times the reads; MetalFX and the
  bilinear composite hide the half-resolution steps.
- **Marching the depth itself.** The first version read 32 full-resolution
  depths per texel and cost 1.18 ms in `Tiled.SunShafts` on the timing view
  below; the R8 source with filtered samples cut the reads' footprint by
  about sixteen times. A runtime branch in the atmosphere draw instead of a
  variant cost 0.07 ms in frames without shafts.

## Verification

2026-10-22, Apple M1 Pro, Release, Metal validation variables unset.

- Build: `./build_editor.sh Release`,
  `VKR_BUILD_TARGET=vkr_harness ./build.sh Release` and
  `VKR_BUILD_TARGET=vulkan_renderer_tester ./build.sh Release`; the full
  `build_release/tests/vulkan_renderer_tester` passed. The MSL compiles at
  renderer start; the Bistro runs below created every pipeline and passed
  their ABI checks.
- Pixels: `local.tiled.bistro_sun_shafts_capture` and
  `local.tiled.bistro_sun_shafts_off_capture` (1920×1080, render scale 0.6,
  MetalFX, automatic exposure; fixtures
  `bistro_tiled_sun_shafts_local.scene.json` with the fog at density 0.02,
  g 0.6, and its `_off_` copy) under `tools/profiles/local-offscreen.json`.
  With the fog off, the `final_color` and `scene_color` captures after the
  change equal those before it (SHA-256 identical). With the fog on, the
  sun's glow and the shafts through the vine show; densities 0.01 and 0.05
  span faint to strong without clipping the sky.
- Timing: see [Cost](#cost).

## Cost

Matched capture-free Release runs on 2026-10-22, Apple M1 Pro, of
`local.tiled.bistro_sun_shafts_timing` (fog on) and
`local.tiled.bistro_sun_shafts_off_timing` (fog off): 2560×1440 hidden
window, immediate present, render scale 0.6 with MetalFX (internal
1536×864, shafts 768×432), 400 warmup and 300 measured frames, profile
`tools/profiles/performance-windowed-gpu.json` (five children, all four
reports authoritative, clean trees, thermal level 0, memory pressure at
most warn). Before is `92d618d1` with the cases added, after the same with
this change, built in one worktree. The fixtures carry no lightmap set, so
Bistro's lamps draw as dynamic lights and `Tiled.Opaque` takes about
30.9 ms; the frame is GPU-bound at about 40 ms.

| Run | `Tiled.SunShafts` mean (children) | `Tiled.Atmosphere` mean | `frame.wall` p50 |
|---|---|---|---|
| Fog on, before | — | 0.785 ms | 40.07 ms |
| Fog on, after | 0.321 ms (0.318–0.327), p95 0.334 | 0.862 ms | 40.46 ms |
| Fog off, before | — | 0.788 ms | 40.05 ms |
| Fog off, after | — | 0.784 ms | 39.91 ms |

The shafts cost 0.32 ms in their pass and 0.08 ms in the atmosphere
draw's variant, about 0.4 ms in all; `vkr_harness perf-compare` puts the
fog-on `frame.wall` change at +0.40 ms (+1.0%), within its threshold, and
the fog-off change at −0.14 ms. Metal's per-pass timestamps split the
shared compute encoder, so the pass row carries about 10 µs of encoder
overhead. At 1920×1080 the shafts cover 2.25 times fewer texels. Report
digests: fog on before `sha256:2509c028…`, after `sha256:c6bab062…`; fog
off before `sha256:02708543…`, after `sha256:10fb7626…`.

## Revisit when

- The tiled class gains a cheap light-space volume (a froxel grid sized
  for tile memory, or shadow-map ray marching at low resolution) that can
  shadow the medium off the screen.
- Shafts are needed from local lights or with the sun off the screen.
