---
status: proposed
updated: 2026-10-21
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
  `Shadow.LocalMask` at 2.1 to 4.2 ms and the redraws at 0.23 ms or less,
  and local shadows cost 3.0 and 6.0 ms in all
  ([High gate costs](#high-gate-costs)).
  Ray-traced sun shadows join High at 1.5 ms or less, and 3 ms is the limit
  for any preset.
- **Host.** The Windows host's only display is 1920×1080; a hidden window
  takes the size it asks for
  ([`vkr_window_windows.c`](../../runtime/src/platform/vkr_window_windows.c)).
  Peer sessions share the checkout and keep it dirty, so authoritative runs
  come from a separate worktree at the commit under test, with Bistro's
  untracked assets linked in.

## High gate costs

Each variant turns one desktop High gate the way its Graphics setting does and
keeps the rest of the case. The variants are copies of the two tracked cases
with one renderer field changed, in the ignored `tools/cases/rerun/`; filtered
far shadows go off through `VKR_SHADOW_EVSM=0`, which the workload
fingerprint records. Measured on 2026-10-21 on the RX 6700 XT, Release, from a
clean worktree of the case-controls commit (75dc93fc on main; the measured
tree is that commit before its rebase onto 52b37401 and c327aada, which
change render graph debug names and the editor's graph view), with
`vkr_harness profile --case <case> --profile
tools/profiles/performance-windowed-gpu-submission-vulkan.json`. Every report
is authoritative.

The reference is the mean of the two High runs that bracket the variants:
18.15 ms median and 23.50 ms p95 on the orbit (reports `a2d153e1`,
`2ecdec11`), 22.06 and 26.25 ms on the walk (`a0052409`, `fab8a6df`). The two
High runs of a case differ by 0.1 ms or less at the median, so a change under
0.2 ms is noise. Each variant has its own workload fingerprint, so
`vkr_harness perf-compare` refuses the pairs (`workload_fingerprint_differs`,
exit 4) and the change below is read from the reports.

| Gate, from High | Case change | Orbit median / p95 | Walk median / p95 |
|---|---|---|---|
| Local shadows off | `shadow_local: false` | −3.01 / −6.56 (`2e49b3ae`) | −5.98 / −8.24 (`4069fd99`) |
| Ambient occlusion off | `gtao_enabled: false` | −1.52 / −1.52 (`a3f52518`) | −1.32 / −1.29 (`81bd632d`) |
| Soft shadows off | `shadow_soft: false` | −0.41 / −0.64 (`aba1825b`) | −0.56 / −0.48 (`3cd2fef2`) |
| Shadow quality Medium, High's filtered far shadows kept | `shadow_preset: balanced`, `shadow_cascades: 3`, `VKR_SHADOW_EVSM=1` | −0.03 / −0.78 (`0b82aeb5`) | −0.56 / −0.76 (`1b12eb57`) |
| Bloom off | `bloom_enabled: false` | −0.16 / −0.17 (`1c998d9f`) | −0.06 / −0.01 (`566d06d1`) |
| Reflection probe off | `ibl_probe_limit: 0` | −0.05 / −0.13 (`6cd47230`) | +0.05 / +0.01 (`36d3fa7e`) |
| Filtered far shadows off | `VKR_SHADOW_EVSM=0` | +0.04 / +0.07 (`daca55f9`) | +0.15 / +0.20 (`e4d3bddd`) |
| View distance Medium | `draw_distance_scale: 0.75` | +0.04 / 0.00 (`4e809034`) | +0.11 / +0.14 (`a083e42f`) |
| SMAA T2x instead of FSR, lever (c) | `antialiasing: smaa_t2x` | −0.68 / −0.75 (`287b82be`) | −0.69 / −0.63 (`ab7a7f0a`) |

Values are the change in `gpu.submission` in milliseconds, with the report
digest prefix in brackets. Per-pass medians come from
`performance-windowed-gpu.json` with `--pass-groups
tools/budgets/desktop.pass_groups.json`, against a High run of the same
profile:

- **Local shadows.** Orbit 3.0 ms: `Shadow.LocalMask` 2.05,
  `Lighting.Deferred` 0.54 for sampling the mask, glass and blend 0.26
  (transmission shading), local shadow-map draws 0.15 (reports `ff32f813`
  High, `fe60ff2e` off). Walk 6.0 ms: `Shadow.LocalMask` 4.19,
  `Lighting.Deferred` 1.34, glass and blend 0.46 (`0dc54243`, `43c87ac8`).
  Without local shadows `Lighting.Deferred` still takes 6.2 ms on the orbit
  and 6.8 ms on the walk.
- **Ambient occlusion**, orbit. `AO.Evaluate` 1.01, `AO.Denoise` 0.22,
  `AO.DenoiseFinal` 0.21, `AO.PrefilterDepth` 0.07, and 0.29 ms of
  `Lighting.Deferred`; the transmission passes take 0.25 ms more without it
  (`3a48ef2f`).
- **Soft shadows**, orbit. `Lighting.Deferred` 0.24 for the sun's filter,
  `Shadow.LocalMask` 0.09 for the local penumbra (`c0b17997`).
- **SMAA T2x**, orbit. `FSR31.Upscale` 1.84 and `FSR31.Prepare` 0.29 give way
  to `Temporal.Resolve` 0.76 and four SMAA passes, 0.54 together;
  reconstruction and post fall from 2.82 to 2.03 ms (`3bf866e1`).

What the costs show:

- No single gate brings a case under 16.7 ms p95. Local shadows off comes
  closest, at 16.95 ms on the orbit and 18.01 ms on the walk.
- Local shadows hold most of the tail. The p95 sits 5.35 ms above the median
  on the orbit with them and 1.80 ms without; 4.19 and 1.93 ms on the walk.
- Local shadows and ambient occlusion off together would leave the walk near
  16.7 ms p95 if their savings add; the pair was not measured.
- Filtered far shadows, view distance, the reflection probe and bloom cost
  0.2 ms or less in Bistro, so moving them saves nothing measurable here.
  View distance Medium does take effect: it removes 4% of the walk's opaque
  draws (2267 to 2177) without a GPU time change.
- Fog, volumetric fog and subsurface scattering do no work in Bistro and were
  not measured.

## Proposed work

1. **Line targets.** Set a p95 target for each budget line once the first
   lever has moved the frame. Until then the baseline values are the
   reference, and each lever reports every line before and after it.
2. **Levers in order.** (a) Move or retune preset gates, using the
   [measured cost of each gate](#high-gate-costs). No single move meets the
   budget, and on 2026-10-21 the owner kept every gate in High: the one move
   with a large effect, local shadows out of High, takes the shadows of
   Bistro's lamps and still leaves the walk at 18.01 ms p95, and the gates
   that cost 0.2 ms or less save nothing. Lever (a) is closed without a
   change. (b) Cut work inside the largest line, Lighting,
   through its own proposal: `Lighting.Deferred` through
   [lighting efficiency](lighting-efficiency.md) and binning, and
   `Shadow.LocalMask` through
   [local shadow architecture](local-shadow-architecture.md) and shadow
   quality. Local shadows cost 3.0 ms median on the orbit and 6.0 ms on the
   walk, against shadow quality's 3.0 ms target, mostly in the mask and in
   deferred lighting's reads of it. (c) SMAA T2x instead of FSR's
   antialiasing, which [ADR-052](../adr/052-vulkan-fsr31-upscaling.md) lists
   as cheaper but less stable; it measures 0.7 ms cheaper on both cases.
   (d) Only then dynamic resolution,
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
