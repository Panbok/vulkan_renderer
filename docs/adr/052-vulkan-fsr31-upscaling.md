---
status: partial
updated: 2026-10-15
authority: adr
---

# ADR-052: Vulkan FSR 3.1 native antialiasing

## Status

Accepted (partial): FSR 3.1 native antialiasing is the default
antialiasing of the desktop pipeline (Vulkan) since 2026-10-15, with SMAA
T2x ([ADR-037](037-portable-same-resolution-temporal-antialiasing.md)) as
the selectable fallback. The owner asked for it after SMAA T2x, SMAA 4x and
specular filtering left thin geometry aliasing in motion. Partial: the Scene
renders at scale 1 only (no upscaling), and the record below has local
evidence from one Windows host. The tiled pipeline has no temporal
reconstruction ([ADR-087](087-gpu-class-graphics-pipelines.md)).

FSR 3.1 first shipped as an opt-in temporal upscaler and was removed on
2026-10-12 ([Short-history rendering](../proposals/short-history-rendering.md),
section 1): its still images needed a VKR 128-frame convergence pass and SSR
settling rules, and a matched Bistro rotation did not show less thin-detail
variation than portable TAA. The return keeps the SDK and bridge, drops the
upscaling scale and the convergence pass, and replaces the evidence with the
measurements below.

## Context

Two-frame SMAA T2x leaves aliasing on thin geometry in motion: it has two
samples per pixel. A second spatial sample (opt-in SMAA 4x) cost 2.7 ms at
2560 × 1440 for about 9 % less edge shimmer, and filtering specular inputs
changed little. More samples per pixel need a longer, validated temporal
reconstruction.

## Decision

`VkrFrameGlobals.antialiasing_mode` selects the desktop antialiasing:
`VKR_ANTIALIASING_MODE_FSR_NATIVE` (zero, the default; graphics setting
`anti_aliasing_method` = `fsr`, "Anti-aliasing method" in the editor's
Preferences) or `VKR_ANTIALIASING_MODE_SMAA_T2X` (`smaa_t2x`). A
frame runs FSR when that mode is selected, temporal antialiasing is on, the
camera is a perspective one and the build and device support FSR; otherwise
SMAA T2x runs.

- **SDK.** FidelityFX SDK `v1.1.4` (FSR 3.1.4, commit `c6efa6bf`) is fetched at
  configure time on Windows and patched
  ([`vendor/fidelityfx-sdk-1.1.4.md`](../../vendor/fidelityfx-sdk-1.1.4.md),
  [`cmake/vkr_fsr.cmake`](../../cmake/vkr_fsr.cmake)). A C++ bridge owns the SDK
  context, its scratch arena and shared images
  ([`vkr_vulkan_fsr_sdk.cpp`](../../renderer/src/vulkan/vkr_vulkan_fsr_sdk.cpp)).
  The device requests `shaderStorageImageExtendedFormats`; without it the
  renderer reports FSR unavailable and runs SMAA T2x.
- **Jitter.** FSR frames take FSR's eight-phase Halton (2, 3) sequence in
  [−0.5, 0.5) pixels (`vkr_temporal_fsr_jitter`) instead of T2x's two phases.
- **Passes.** `FSR31.Prepare` writes FSR's depth, reactive mask (transmission
  and blended layers) and composition mask from the Scene's HDR colour, its
  colour before transmission, temporal validity and depth
  ([`post/fsr31.slang`](../../renderer/src/shaders/vulkan/slang/post/fsr31.slang)).
  `FSR31.Upscale` records the SDK dispatch at render scale 1 into
  `scene_resolved_color`, the image `Temporal.Resolve` writes on T2x frames,
  so exposure, bloom, depth of field, motion blur and the display transform
  read it unchanged. Both run in full-screen frames and in the editor
  viewport. `Temporal.Resolve` and every SMAA pass are skipped; RCAS
  sharpening stays off.
- **History.** The SDK keeps its own history; a frame without valid temporal
  history, an output resize and any frame without FSR reset it. A change of
  mode resets temporal history, so T2x never reads the depth, identity and
  surface history that FSR frames leave unwritten; motion still anchors on
  those instances. A cancelled submission recreates
  the SDK context, whose CPU state it advanced.

## Consequences

Evidence, Vulkan Release, RX 6700 XT, 2026-10-15, local and
non-authoritative (dirty tree; warmup stability not met), against the
Bistro street view of `local_shadow_bistro_street_capture` at 1280 × 720 and
the same frames rendered at 3840 × 2160 without antialiasing, box-filtered
3 × 3 in linear light (mean absolute 8-bit error; the oracle is described in
ADR-037):

| Case | Measure | SMAA T2x | FSR |
|---|---|---|---|
| Still | geometric edges / elsewhere | 10.27 / 4.37 | 9.66 / 4.05 |
| Camera turning 2°/s | geometric edges | 11.41 | 10.48 |
| | edge instability (σ of the error over 4 frames) | 8.63 | 7.73 |
| | thin features | 9.08 | 8.41 |
| | pixels an edge just crossed | 15.31 | 13.23 |
| | edge flicker (mean second difference; reference 12.61) | 12.79 | 6.53 |
| Strafing 1.5 m/s | geometric edges | 11.86 | 10.29 |
| | pixels an edge just crossed | 9.49 | 8.04 |

FSR flickers less than the reference's own motion, so part of its gain is
temporal smoothing: motion is slightly softer, not only steadier. Crops of
the strafe show no trails, and mean brightness matches.

Timing, `profile` with `local-offscreen-gpu-repeated.json` at 2560 × 1440,
5 × 60 frames, SMAA T2x then FSR within ten minutes: frame wall 20.40 →
21.02 ms p50; pass sum 19.93 → 20.72 ms. FSR's chain (`FSR31.Prepare` 0.28,
`FSR31.Upscale` 1.86, tonemap 0.20 ms) replaces T2x's 1.50 ms
(`Temporal.Resolve` 0.57 plus the display-linear, SMAA and resolve passes).
Debug with synchronization validation on the street view: no messages.

The default changes the image of every desktop frame with temporal
antialiasing. Accepted harness baselines recorded with SMAA T2x no longer
match until the owner accepts new ones; a case selects either mode with
`renderer.antialiasing` (`fsr` by default, or `smaa_t2x`), which joins the
workload fingerprint only with temporal antialiasing on. Harness summaries are version 19; version 18 and older
read back as `smaa_t2x`.

## Alternatives considered

- **SMAA 4x and a four-phase T2x** (ADR-037): S2x cost 2.7 ms for less gain;
  the still-camera bound of four temporal phases was 8 % on edges, with
  multi-frame reprojection still to build.
- **The 2026-10-12 FSR upscaler with its convergence pass**: a 128-frame
  stationary average hid stochastic shading behind long history, which the
  owner rejected; native AA without it is measured above.
- **DLSS or XeSS**: vendor-specific; FSR runs on every desktop GPU this
  renderer supports.

## Revisit when

Ghosting or smearing shows on moving objects (deformation and particles
carry no motion contract yet), FSR's softness in motion outweighs its
stability, the desktop budget needs a render scale below 1, or a release
expects DLSS or XeSS.
