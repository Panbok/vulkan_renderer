---
status: proposed
updated: 2026-10-20
authority: proposal
---

# Desktop frame budget

The desktop pipeline, which Vulkan runs, has a frame budget and an accepted
first baseline ([ADR-087](../adr/087-gpu-class-graphics-pipelines.md),
decision 4 and
[Desktop pipeline baseline](../adr/087-gpu-class-graphics-pipelines.md#desktop-pipeline-baseline)):
Bistro with the High preset at 2560×1440 on the RX 6700 XT, 16.7 ms p95
`gpu.submission`. The baseline misses it, at 23.76 ms p95 on the orbit and
26.68 ms on the street walk. This proposal holds the work that remains:
targets per budget line and the levers that bring the frame under the
budget. [Asynchronous compute](async-compute.md),
[variable-rate shading](variable-rate-shading.md) and
[GPU light binning](clustered-light-and-decal-binning.md) are speed
features; each claims its gain against the baseline with its own matched
reports.

## Current baseline

- **Measurement.** Two tracked cases pin Vulkan, 2560×1440 physical pixels,
  a hidden window with immediate presentation and the High preset:
  [`bistro_vulkan_high_orbit_1440`](../../tools/cases/performance/bistro_vulkan_high_orbit_1440.case.json)
  for steady cost and
  [`bistro_vulkan_high_street_walk_1440`](../../tools/cases/performance/bistro_vulkan_high_street_walk_1440.case.json)
  for shadow-redraw tails.
  [`performance-windowed-gpu-submission-vulkan.json`](../../tools/profiles/performance-windowed-gpu-submission-vulkan.json)
  measures `gpu.submission` with five children on a clean tree, pinned to
  the RX 6700 XT, and `performance-windowed-gpu.json` with
  `--pass-groups` and
  [`desktop.pass_groups.json`](../../tools/budgets/desktop.pass_groups.json)
  reports the budget lines
  ([ADR-051](../adr/051-renderer-harness-and-evidence.md)).
- **Where the time goes.** At the median, Lighting takes 8.8 ms on the orbit
  and 12.3 ms on the walk: `Lighting.Deferred` 6.7 and 8.1 ms,
  `Shadow.LocalMask` 2.1 and 4.2 ms. Reconstruction and post take 2.9 ms
  (`FSR31.Upscale` 1.9), Surface 2.7 (`GBuffer.Resolve` 1.1, `AO.Evaluate`
  1.0), Glass and blend 2.4 and 2.9. The shadow-map redraws stay at 0.23 ms
  or less (ADR-087).
- **Cases carry the preset.** A harness case reads no Graphics settings and
  has no preset field (`vkr_harness_child_apply_renderer` in
  [`vkr_harness_child.c`](../../tools/harness/vkr_harness_child.c)), and it
  leaves bloom and GTAO off unless it turns them on. The two cases spell out
  High gate by gate. Soft and local shadows and the draw distance
  (`shadow_soft`, `shadow_local`, `draw_distance_scale`), reflection probes
  (`ibl_probe_limit`) and filtered far shadows (the shadow preset) default to
  High's values. Fog, volumetric fog and subsurface scattering have no case
  field; Bistro has no fog, froxel volume or subsurface data, so they do no
  work there. These variables change gates without a
  change to the workload fingerprint, which records only `VKR_SHADOW_EVSM`
  and `VKR_POST_TRANSFORM_CACHE`: `VKR_SUN_RAY_SHADOWS`,
  `VKR_SUN_CONTACT_SHADOWS`, `VKR_BLOOM_DISABLED`, `VKR_GTAO_DISABLED`,
  `VKR_DRAW_DISTANCE`, `VKR_SHADOW_FAR_DISABLED`,
  `VKR_LOCAL_SHADOW_FADE_DISTANCE`, `VKR_LOCAL_SHADOW_ATLAS_LAYERS` and
  `VKR_RAY_QUERY_DISABLED`. Budget runs leave them unset.
- **Feature budgets.** The dynamic diffuse layer has 1.0 ms at 2560×1440
  ([ADR-119](../adr/119-dynamic-diffuse-volume-layer.md)). The
  [shadow quality](shadow-quality.md#budgets) targets for Bistro High at
  1440p are sun 2.0 ms median, 0.5 ms extra in any one frame and local
  shadows 3.0 ms median against an estimated 6.3 ms; the baseline measures
  `Shadow.LocalMask` at 2.1 to 4.2 ms and the redraws at 0.23 ms or less.
  Ray-traced sun shadows join High at 1.5 ms or less, and 3 ms is the limit
  for any preset.
- **Host.** The Windows host's only display is 1920×1080; a hidden window
  takes the size it asks for
  ([`vkr_window_windows.c`](../../runtime/src/platform/vkr_window_windows.c)).
  Peer sessions share the checkout and keep it dirty, so authoritative runs
  come from a separate worktree at the commit under test, with Bistro's
  untracked assets linked in.

## Proposed work

1. **Line targets.** Set a p95 target for each budget line once the first
   lever has moved the frame. Until then the baseline values are the
   reference, and each lever reports every line before and after it.
2. **Levers in order.** (a) Move or retune preset gates, using a measured
   cost for each gate. (b) Cut work inside the largest line, Lighting,
   through its own proposal: `Lighting.Deferred` through
   [lighting efficiency](lighting-efficiency.md) and binning, and
   `Shadow.LocalMask` through
   [local shadow architecture](local-shadow-architecture.md) and shadow
   quality. (c) SMAA T2x instead of FSR's antialiasing, which
   [ADR-052](../adr/052-vulkan-fsr31-upscaling.md) lists as cheaper but less
   stable; FSR's chain costs 1.9 ms here. (d) Only then dynamic resolution,
   which needs a Vulkan scene scale and an FSR upscale mode and reopens
   ADR-052; the owner avoids it unless the budget cannot be met at a fixed
   resolution, because a changing resolution blurs the image in motion.
3. **120 Hz.** A 120 Hz line for High can follow once High fits 60 Hz. Epic
   has no budget, in line with the owner's "as far as the hardware allows"
   (ADR-087, Context).

## Evidence needed

- For each lever, matched before and after reports of both cases under
  `performance-windowed-gpu-submission-vulkan`, and the pass-group report for
  the line it targets, each from a clean revision on the same host, with a
  `vkr_harness perf-compare` verdict.
- Equal work-volume rows, and equal `depth` and `gbuffer_*` captures, unless
  the lever changes output on purpose; a change to the High preset's look is
  the owner's decision.
- Release from the repository wrapper, with the graphics validation and gate
  variables unset and one GPU process on the host. Bistro only.

## Risks

- One device sets the budget; RDNA 3, Ampere and newer GPUs have no native
  evidence ([ADR-083](../adr/083-supported-hardware-matrix.md)), so the
  budget does not transfer between vendors.
- The orbit and walk may miss content-driven peaks, such as many lights in
  view; the binning and shadow proposals need their own cases.
- The cases copy the High preset field by field. When
  `graphics_preset_gates` in
  [`vkr_graphics_settings.c`](../../runtime/src/vkr_graphics_settings.c)
  changes High, the two cases must change with it.
- A hidden window never reaches the display; the presentation cost of a
  visible full-screen window is not in the measurement.
