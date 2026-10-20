---
status: implemented
updated: 2026-10-16
authority: adr
---

# ADR-042: Scene-linear exposure, bloom and ambient visibility

## Status

Accepted. Both pipeline classes run exposure and bloom. GTAO and temporal
history belong to the desktop pipeline, which Vulkan implements
([ADR-087](087-gpu-class-graphics-pipelines.md), decision 7).

## Context

Post effects must preserve temporal radiance, expose independent bypasses and
modify the lighting terms they physically approximate.

## Decision

Keep temporal history scene-linear, up to the whole-stop pre-exposure of
[ADR-081](081-physical-night-sky.md), which every history consumer rescales
exactly. Meter the post-temporal HDR source with a 256-bin log-luminance
histogram over [-24, 8] log2 and percentile exposure resolve. Renderer-owned
frame delta/discontinuities control completion-safe EV history. Each native
renderer advances an exposure clock only when an exposure output is submitted
and stores that clock with the output. Adaptation uses elapsed exposure-clock
time since the selected completed history, bounded by the shared hitch limit;
it does not apply only one frame's delta to an older state. Vulkan passes the
selected completed state to the resolve kernel as a host copy from that frame's
readback, uploaded with the frame's roots
([`vkr_vulkan_deferred.c`](../../renderer/src/vulkan/vkr_vulkan_deferred.c)). A
GPU read of the old state instance keeps it in use until the reading frame
completes. The oldest instances are the ones the shared history ring reuses, so
with temporal history also live the ring waited for an in-flight frame every
third frame. In the Bistro street view at 1920x1080 with TAA on the RX 6700 XT,
frames alternated 10, 20 and 2 ms (p95 20.5 ms); with the host copy they take
11.0 ms (p95 11.5 ms) and the final color is unchanged. Metal still reads the
state instance on the GPU
([`vkr_metal_packet_graph.inc`](../../renderer/src/metal/internal/vkr_metal_packet_graph.inc)):
with two frame slots and a four-instance ring, once a frame waits for its slot
only the previous frame is in flight, so the oldest instance's last reader has
completed and selection does not wait. On the Metal desktop implementation,
removed on 2026-10-06, `local_shadow_taps_bistro_metal_street_taa` (a Metal
desktop case since removed; M1 Pro, 1280x720, TAA, two children of 300 frames)
read `frame.wall` p50 14.33 and p95 16.02 ms with no repeating pattern. Invalid
history snaps to target. Defaults lower exposure at 8 EV/s, raise it at 1 EV/s,
and clamp the target to [-8,+24] EV so night scenes are reachable. The rate
names describe displayed-image brightness. Tonemap consumes GPU state without
synchronous CPU readback; delayed completed samples expose diagnostics. Manual
exposure remains an explicit alternative. Histogram dispatches use complete
16x16 threadgroups on both backends: all 256 lanes initialize and merge bins,
while edge lanes omit out-of-extent source reads.

Bloom prefilters scene-linear HDR with threshold/soft knee into a bounded
half-resolution chain, downsamples, accumulates deepest-first, and combines into
full-resolution HDR before exposure multiplication/tonemap; the tiled pipeline
folds the combine into its tonemap passes (ADR-087, decision 6). Separate graph
resources preserve read/write dependencies. Shared arithmetic handles non-finite
and extreme input and pins the knee/Karis behavior. Threshold, knee and
firefly clamp are exposed luminance: 1 is the level the exposure maps to
white. The levels follow the frame's display exposure, the newest completed
automatic exposure or the manual one, which trails the tonemap by the frames
in flight (`vkr_bloom_gpu_params`). Since 2026-10-16 a dark interior and
daylight bloom the same over-white highlights; with scene-linear levels the
Level Design Test's lamps (peak 49) clamped at 32 and bloomed as wide white
discs at the interior's exposure of 7.4. Since 2026-10-19 the firefly clamp
is 8, three stops over white: daylight through a gate gap in a tunnel (peak
31.8 at exposure 6.44, about 205 exposed) clamped at 32 and bloomed over a
third of the screen. Bloom can bypass independently.
Combine intensity is multiplied once by configured maximum mip count divided
by the actual contributing count. This preserves the usual full six-level look
and its constant-field gain on shorter chains. Fewer than two levels disables
bloom. Normalization changes amplitude, not the spatial spread of a shorter
chain; firefly saturation and boundaries remain nonlinear limits of the DC rule.

GTAO uses current-frame depth and normals before deferred lighting. A dedicated
positive-view-depth R16 pyramid feeds full-resolution three-slice/three-step
horizon evaluation and two edge-aware 3x3 denoise passes (`AO.Denoise` into
`gtao_denoised`, `AO.DenoiseFinal` into `gtao_visibility`). The slice and step
noise is one fixed spatial pattern (`vkr_gtao_spatial_noise`), the same every
frame whether or not TAA runs, so no temporal filter has to average it; the two
passes remove it ([Short-history rendering](../proposals/short-history-rendering.md),
section 3). Since 2026-10-14 the pattern is a 4x4 Bayer order over an R2
sequence, and the passes weigh their taps so that together they average one
4-pixel period exactly: the first by a (1, 2, 1) tent per axis, the second
(`pass.gtao.denoise_final`) only by its diagonals, (1, 0, 1) per axis, whose
product is (1, 2, 2, 2, 1). The earlier Hilbert order over a 64x64 tile left
curved streaks under T2x, which averages two frames only. Raw, intermediate and denoised outputs are RGBA8: RGB encodes a
world-space bent normal, and alpha holds scalar visibility. The existing R8
edge image remains separate. The graph owns these transient images per
in-flight image; the three RGBA8 images take 10.5 MiB at 1280x720, or 31.6 MiB
across three image sets. Pass scheduling and sample counts stay fixed.
Depth-pyramid horizon samples use nearest texel and nearest mip filtering.
Slice directions use the signed view-space pixel scale, including projection Y,
and the positive/negative horizon bounds share the integration sign convention.
The denoiser averages decoded directions and visibility before normalizing and
repacking. Lighting blends the bent direction toward the shading normal as
visibility approaches one. Full visibility preserves the original lighting,
including the disabled fallback, for every reflection direction.

Global/probe diffuse uses the bent normal and albedo-aware multi-bounce AO fit,
multiplied by independent material AO. This fit compensates local missing energy;
it does not compute scene-wide light transport or color bleeding. Baked diffuse
volumes retain their shading-normal SH lookup and scalar AO because their bake
already includes multiple bounces. Indirect specular uses a visibility cone
derived from alpha and an approximate spherical-cap intersection with the rough
reflection lobe. The practical near-mirror fade limits this approximation.
The environment receiver weight applies the cone factor once; authored IBL
gain remains specific to environment lighting.
Direct lighting keeps its shadow visibility. GTAO is not reused HZB history or
general wall visibility. Raw/denoised capture schema version 2 records the RGBA
directional tuple as data, without a color-transfer interpretation.

Automatic exposure, bloom and GTAO are enabled by production initialization.
Authored graph conditions and packet globals retain isolated bypasses and direct
intermediate capture channels. Final output follows ADR-043.

## Consequences

Effects have observable inputs and independent controls. Quality acceptance
and GPU cost still require matched cases on each backend that runs an effect;
enabling effects is a workload change.

On the Metal desktop implementation, removed on 2026-10-06, the Release corner
fixture verified that bent normals point away from a nearby wall and that
diffuse/specular lighting follows the captured tuple (maximum HDR error
0.000960297 across 34 samples). Disabling GTAO preserved the previous fixture's
HDR bytes. A mirror fixture exercised non-neutral cone attenuation of SSR, since
removed (ADR-055), with maximum HDR error 0.002351667 across 15 interior
samples. A directional-SH
volume fixture verified scalar AO across 201,629 covered pixels (maximum HDR
error 0.0002432). Production Vulkan SPIR-V validation passes; native Vulkan
GTAO output is not yet recorded. These checks establish local behavior, not a
frame-time claim.

Spatial GTAO, Vulkan Release, RX 6700 XT, Windows, 2026-10-13, local and
non-authoritative, TAA off, Bistro street (`win_bistro_gtao_on`, 800×600) and
café (`win_bistro_cafe_gtao_on`, 1280×720), `local-offscreen.json`:
- Two identical runs give byte-identical `gtao_raw`, `gtao_visibility` and
  `final_color` in both views.
- Against one denoise pass, the second changes `gtao_visibility` on 46 % and
  51 % of pixels by more than 2/255 (mean 4.1 and 4.0/255) and removes the
  fine grain on floors and chairs; a third changes it by half as much (mean
  2.1 and 1.8/255) and mainly softens small detail, so two passes ship.
- `gtao_raw`, `gtao_view_depth` and `gbuffer_normal` are unchanged: without
  TAA the noise index was already zero.
- `profile` with `local-offscreen-gpu-repeated.json` at 2560×1440, 5 × 60
  measured frames, two alternated rounds: `AO.Denoise` and `AO.DenoiseFinal`
  each take 0.205 ms (p50; σ ≤ 0.012 ms); `AO.Evaluate` 0.92 ms and
  `AO.PrefilterDepth` 0.073 ms, so the chain costs about 1.40 ms.
- 4x4 noise with the exact-period passes (2026-10-14, same cases): the fine
  grain on the café's counter and floor is gone and edges stay; the mean
  absolute difference of `gtao_visibility`'s alpha from its 9x9 mean fell
  from 10.72 to 9.53 (café) and 11.44 to 10.54 (street), 95th percentile
  42.3 to 37.7 and 42.9 to 39.5; the pass count and taps are unchanged.
- Bistro's materials carry no AO (the café's G-buffer AO is never below
  248/255), so `min(material AO, GTAO)` and the product give the same image
  there; the product stays until a scene with authored AO decides it.
- No GTAO grain shows on the café's metal and glossy surfaces with two
  passes, so the specular cone is unchanged (audit R9).

## Alternatives considered

Continuously pre-exposing temporal history couples adaptation to
reconstruction; ADR-081 instead steps pre-exposure in whole stops with
hysteresis and rescales histories exactly. Scalar AO on all
lighting darkens direct lighting and cannot model directional specular occlusion. Reusing stale HZB as current
GTAO depth changes the input contract.

## Revisit when

Accepted image-quality fixtures require a different metering, bloom or AO model,
or matched Release cost justifies changing resolution/quality.

## Implementation

[`vkr_exposure.c`](../../renderer/src/vkr_exposure.c),
[`vkr_bloom.c`](../../renderer/src/vkr_bloom.c),
[`vkr_gtao.c`](../../renderer/src/vkr_gtao.c),
[`shared/`](../../renderer/src/shaders/shared), and
[`main.rendergraph.json`](../../assets/render_graphs/main.rendergraph.json).
