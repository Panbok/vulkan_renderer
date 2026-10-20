---
status: proposed
updated: 2026-10-20
authority: proposal
---

# Desktop frame budget

The desktop pipeline, which Vulkan runs, has a frame budget since
2026-10-20 ([ADR-087](../adr/087-gpu-class-graphics-pipelines.md),
decision 4). This proposal holds the work that remains: the cases and
profile that measure it, budget lines per pass group, the levers for a
miss, and the first authoritative Bistro baseline.
[Asynchronous compute](async-compute.md),
[variable-rate shading](variable-rate-shading.md) and
[GPU light binning](clustered-light-and-decal-binning.md) are speed
features; each needs this baseline before it can claim a gain.

## Current baseline

- **Budget.** Bistro at 2560×1440 physical pixels on the RX 6700 XT, High
  preset: 16.7 ms p95 `gpu.submission`, and `frame.wall` p95 under the same
  16.7 ms (owner decisions, 2026-10-20, recorded in ADR-087). The RX 6700 XT
  is the only Vulkan device with native evidence; a lower floor (an RX 6600
  class card) would need its own host. 2560×1440 is the tiled budget's
  output resolution, but the tiled Scene renders 0.6 of each axis under
  MetalFX ([ADR-121](../adr/121-tiled-temporal-upscaling.md)), so the two
  classes shade different pixel counts. Epic has no budget, in line with the
  owner's "as far as the hardware allows" (ADR-087, Context); a 120 Hz line
  can follow once High fits. Dynamic resolution is the last lever, because a
  changing resolution blurs the image in motion, when gameplay needs it
  sharpest.
- **Device and host.** [ADR-083](../adr/083-supported-hardware-matrix.md)
  names the RX 6700 XT 12 GB, Windows 10, driver 26.6.3. The Windows host
  pairs it with a Ryzen 5 2600 and 16 GB of RAM
  ([verification record](windows-vulkan-verification.md)); its one display
  is 1920×1080. Harness reports name the device `AMD Radeon RX 6700 XT` and
  the driver `26.6.3 (AMD proprietary shader compiler)`.
- **Scale and antialiasing.** Vulkan renders the Scene at unit scale and
  rejects any other scale; FSR 3.1 runs as native antialiasing at that scale
  ([ARCHITECTURE.md](../ARCHITECTURE.md#presentation-and-platform-boundaries),
  [ADR-052](../adr/052-vulkan-fsr31-upscaling.md)). The Graphics settings
  default to the High preset, unit render scale and dynamic resolution off
  on Vulkan; the dynamic-resolution controller holds 90% of its target frame
  time and serves the tiled pipeline only
  ([`vkr_graphics_settings.c`](../../runtime/src/vkr_graphics_settings.c),
  `VKR_GRAPHICS_DYNAMIC_RESOLUTION_BUDGET_SHARE` in
  [`vkr_graphics_settings.h`](../../runtime/src/vkr_graphics_settings.h),
  [`vkr_sample_runtime_config.c`](../../runtime/src/vkr_sample_runtime_config.c)).
- **Presets in cases.** A harness case does not read the Graphics settings
  and has no preset field; `vkr_harness_child_apply_renderer` in
  [`vkr_harness_child.c`](../../tools/harness/vkr_harness_child.c) applies
  the case's renderer block. A case reaches High gate by gate: with the
  fields left out, bloom and GTAO are off
  ([`vkr_harness_manifest.c`](../../tools/harness/vkr_harness_manifest.c)),
  so no tracked performance case ran High before 2026-10-20. Soft, local
  and filtered far shadows, fog, volumetric fog, subsurface scattering,
  reflection probes and view distance have no case field and already take
  High's values.
  `shadow_preset` `"high"` and `"default"` both select High's shadow
  configuration; `"ultra"` raises the local shadow budgets and adds
  local contact shadows. These variables change gates without a change to
  the workload fingerprint, which records only
  `VKR_SHADOW_EVSM` and `VKR_POST_TRANSFORM_CACHE`: `VKR_SUN_RAY_SHADOWS`,
  `VKR_SUN_CONTACT_SHADOWS`, `VKR_BLOOM_DISABLED`, `VKR_GTAO_DISABLED`,
  `VKR_DRAW_DISTANCE`, `VKR_SHADOW_FAR_DISABLED`,
  `VKR_LOCAL_SHADOW_FADE_DISTANCE`, `VKR_LOCAL_SHADOW_ATLAS_LAYERS` and
  `VKR_RAY_QUERY_DISABLED`.
- **Timing instruments.** Vulkan records whole-submission GPU time from two
  frame timestamps
  ([`vkr_vulkan_renderer.c`](../../renderer/src/vulkan/vkr_vulkan_renderer.c)).
  Until 2026-10-20 the harness lost them on a GPU-bound frame: with all three
  frame slots in flight a slot was reused before the harness polled it, and
  the High orbit kept 3 of 300 samples. The backend now keeps completed
  results until polled, and the orbit keeps 300 of 300
  ([ADR-051](../adr/051-renderer-harness-and-evidence.md)).
  Both Metal submission profiles pin `"driver": "Metal 4"`
  ([`performance-windowed-gpu-submission.json`](../../tools/profiles/performance-windowed-gpu-submission.json),
  `local-windowed-gpu-submission-single.json`), a pin from when only Metal
  collected submission timings.
  [`performance-windowed-gpu.json`](../../tools/profiles/performance-windowed-gpu.json)
  measures per-pass timestamps on either backend. On Metal they split each
  shared compute encoder and inflate compute rows
  ([vkr-performance](../../.codex/skills/vkr-performance/SKILL.md)); their
  cost on Vulkan is not measured.
- **Cases.** Most tracked Bistro performance cases leave the backend
  unpinned. A windowed case's size is in points on macOS and in physical
  client pixels on Windows, so `bistro_material_orbit_1440` is 2560×1440
  pixels on a 2x Mac display and 1280×720 on the Windows host
  ([case](../../tools/cases/performance/bistro_material_orbit_1440.case.json)).
  The four `bistro_shadow_quality_*` performance cases pin Vulkan, offscreen
  at 1280×720. The other Vulkan Bistro cases are local, among them the street
  view, the four-viewpoint walk
  ([`local_shadow_bistro_vulkan_walk`](../../tools/cases/local/local_shadow_bistro_vulkan_walk.case.json))
  and the large-level glide. Some local ones set High's gates, all
  offscreen: the large-level walk, glide and overview at 1280×720 and the
  `shadow_bistro_sun_ray_*_perf` cases at 2560×1440, which a variable
  switches to ray-traced shadows.
- **Window size.** Windows limits a captioned window to the desktop plus its
  frame (`SM_CXMAXTRACK`, 1940×1100 on the host). Before 2026-10-20 a hidden
  window asked for a 2560×1440 client got 1924×1061, and `vkr_window_resize`
  failed
  ([`vkr_window_windows.c`](../../runtime/src/platform/vkr_window_windows.c)).
- **Recorded Vulkan costs.** All are local and non-authoritative. Bistro
  street view at 1920×1080, High: `Lighting.Deferred` 3.50 to 3.69 ms,
  `Shadow.LocalMask` 2.82 ms, transmission 1.50 ms
  ([lighting efficiency](lighting-efficiency.md)). The street view offscreen
  at 2560×1440 (`local-offscreen-gpu-repeated`, dirty tree, warmup not
  stable): frame wall 20.40 ms p50 with SMAA T2x and 21.02 ms with FSR
  (ADR-052), over the budget at the median. In a Windows editor flight over
  the streamed 8 km terrain, not Bistro, the slowest frames were local
  shadow redraws (verification record, H7). The first authoritative
  High measurements are under step 5.
- **Feature budgets.** The dynamic diffuse layer has 1.0 ms at 2560×1440
  ([ADR-119](../adr/119-dynamic-diffuse-volume-layer.md)). The
  [shadow quality](shadow-quality.md#budgets) targets for Bistro High at
  1440p: sun 2.0 ms median, 0.5 ms extra in any one frame, local shadows
  3.0 ms median against about 6.3 ms (scaled from 1080p, not measured);
  the step 5 baseline measures `Shadow.LocalMask` at 2.1 to 4.2 ms and the
  shadow-map redraws at 0.2 ms or less at the median.
  Ray-traced sun shadows join High at 1.5 ms or less, and 3 ms is the limit
  for any preset.

## Proposed work

1. **Budget.** Done 2026-10-20: ADR-087 decision 4.
2. **Cases and profile.** Added 2026-10-20, not run yet:
   - [`bistro_vulkan_high_orbit_1440`](../../tools/cases/performance/bistro_vulkan_high_orbit_1440.case.json)
     follows the material orbit for steady cost (120 warmup and 300
     measured frames).
     [`bistro_vulkan_high_street_walk_1440`](../../tools/cases/performance/bistro_vulkan_high_street_walk_1440.case.json)
     follows the four street viewpoints for shadow-redraw tails (240 and
     1086 frames). Both pin Vulkan and 2560×1440 physical pixels, run a
     hidden window with immediate presentation and full boot, and spell out
     High in their renderer block.
   - [`performance-windowed-gpu-submission-vulkan.json`](../../tools/profiles/performance-windowed-gpu-submission-vulkan.json)
     (authoritative, five children) and
     [`local-windowed-gpu-submission-vulkan-single.json`](../../tools/profiles/local-windowed-gpu-submission-vulkan-single.json)
     pin `"gpu": "AMD Radeon RX 6700 XT"`. The Metal profiles keep their
     pin, so their policy fingerprint, which the Mac results cite, does not
     change.
   - The Windows window procedure raises a hidden window's maximum track
     size to the frame `vkr_window_resize` asked for (`WM_GETMINMAXINFO`),
     so a 2560×1440 client fits on the 1920×1080 host. A visible window
     keeps the desktop limit.

   Both cases ran once each with the local profile on 2026-10-20 (Release,
   dirty tree with peer sessions' renderer edits, one child, no timing
   claim). Each realized 2560×1440 with immediate presentation, stable
   warmup, every `gpu.submission` sample valid and no GPU or log error. The
   pass set is High's: `AO.*`, `Post.Bloom.*` and `FSR31.*` run,
   `Shadow.SunContact` and `Shadow.SunRay*` do not; no `Froxel.*` pass runs
   because Bistro has no froxel fog volume. The orbit passes
   (`sha256:59aa55c1…`). The walk fails one assertion in two runs
   (`sha256:6c33184a…`, `35606b2f…`): `visibility.gbuffer.resolve_invalid`
   counts one rejected surface at frame 319, the same frame each time, and
   does so on the clean commit `9ef1c728` too, so it predates the peer
   edits. It must be explained or fixed before the walk can pass; the
   assertion stays.
3. **Budget lines.**
   [`desktop.pass_groups.json`](../../tools/budgets/desktop.pass_groups.json)
   groups the graph passes, and `vkr_harness profile --pass-groups <file>`
   reports each group's per-frame GPU sum (median, p95) in
   `aggregate.pass_groups`. A pass belongs to the group with its most
   specific pattern; a pass that matches none goes to `unassigned`. Passes
   can overlap on the GPU, so the lines do not add up to `gpu.submission`.
   Set each line's target from the first baseline. The baseline runs below
   assigned every pass and left none unassigned; the orbit's line medians
   sum to 18.1 ms against an 18.5 ms `frame.wall` median.

   | Line | Graph passes |
   |---|---|
   | Culling | `Cull.*`, `Transmission.Cull.*`, `Animation.Skinning` |
   | Shadows | `Shadow.Cascade.*`, `Shadow.Far.*`, `Shadow.Local.*`, `Shadow.Moments.*` |
   | Visibility | `VBuffer.*`, `HZB.*`, `SDSM.Reduce` |
   | Surface | `GBuffer.Resolve.*`, `AO.*`, `Subsurface.Gather` |
   | Lighting | `Shadow.LocalMask`, `Shadow.Sun*`, `Volume.*`, `DiffuseVolume.*`, `IBL.Bake`, `Reflection.Raster`, `Lighting.Deferred.*` |
   | Media | `Sky.*`, `Clouds.*`, `Froxel.*`, `Fog.Apply.*` |
   | Glass and blend | `Transmission.*`, `VBuffer.Transmission.*`, `World.Blend.*`, `World.FeedbackCopy.*` |
   | Reconstruction and post | `FSR31.*`, `Temporal.*`, `Post.*`, `MSAA.Compose`, `UI.*` |
   | Editor and debug, 0 in a budget case | `Editor.*`, `Picking.*`, `Animation.Preview`, `Debug.GpuSpike` |

4. **Levers in order.** When the baseline misses the budget: (a) move or
   retune preset gates, using a measured cost for each gate; (b) cut work
   inside the largest line, Lighting, through its own proposal:
   `Lighting.Deferred` (6.7 to 8.1 ms median) through
   [lighting efficiency](lighting-efficiency.md) and binning, and
   `Shadow.LocalMask` (2.1 to 4.2 ms) through
   [local shadow architecture](local-shadow-architecture.md) and shadow
   quality. The shadow-map redraws themselves (the Shadows line) take
   0.2 ms or less at the median and 0.8 ms at p95; (c) SMAA T2x instead of
   FSR's antialiasing, which ADR-052 lists as cheaper but less stable; (d)
   only then dynamic resolution, which needs a Vulkan scene scale and an FSR
   upscale mode and reopens ADR-052.
5. **Baseline.** Proposed 2026-10-20, awaiting the owner's acceptance
   (AGENTS.md). `vkr_harness baseline` publishes image snapshots only, so a
   timing baseline is its authoritative reports, recorded with their
   digests; once accepted it moves to ADR-087 beside the tiled table. All
   runs: Release, commit `9ef1c728` in a clean worktree (`dirty: false`),
   RX 6700 XT, driver 26.6.3, five children, stable warmup, immediate
   present, exclusive GPU lane, `authoritative: true` with no authority
   reason, no GPU error, no memory pressure.

   | Case | Profile | Status | `gpu.submission` median / p95 / max | Child medians / p95s | Report |
   |---|---|---|---|---|---|
   | Orbit | `performance-windowed-gpu-submission-vulkan` | pass | 18.21 / 23.50 / 24.95 ms | 18.15–18.22 / 23.42–23.51 ms | `sha256:b6add114d3155ea4ffecad8582c4451a01d958648d48def2d2ec24d5a4286285` |
   | Walk | `performance-windowed-gpu-submission-vulkan` | fail (rejected surface) | 22.18 / 26.45 / 35.77 ms | 22.13–22.21 / 26.45–26.46 ms | `sha256:2bf6839db2da01d9df4a7e3f0ee4b806e4399a0d0db33482ad099f8c7d5f7795` |

   `frame.wall` p95 was 23.87 ms (orbit) and 26.77 ms (walk). Both miss
   the 16.7 ms p95 budget, by 6.8 and 9.8 ms. Budget lines, median / p95
   in ms, from `performance-windowed-gpu --pass-groups` (orbit
   `sha256:113f3d8b1aa941dd49ae1cf15b753dc3cbffecd72fa201fb99c30bd5c901e3bf`,
   pass; walk
   `sha256:e98437b29c06955bfd6324286f4dd4f8c35bc7092e63f943f0ad981ec4dd73e0`,
   fail for the same rejected surface):

   | Line | Orbit | Walk |
   |---|---|---|
   | Culling | 0.04 / 0.04 | 0.04 / 0.04 |
   | Shadows | 0.23 / 0.80 | 0.00 / 0.42 |
   | Visibility | 0.58 / 0.68 | 0.66 / 0.84 |
   | Surface | 2.66 / 2.77 | 2.69 / 2.80 |
   | Lighting | 8.78 / 12.88 | 12.30 / 15.38 |
   | Media | 0.54 / 0.67 | 0.57 / 0.66 |
   | Glass and blend | 2.43 / 3.55 | 2.94 / 3.94 |
   | Reconstruction and post | 2.84 / 2.93 | 2.93 / 2.98 |
   | Editor and debug | 0 / 0 | 0 / 0 |

   The largest passes at the median: `Lighting.Deferred.Fullscreen` 6.73
   and 8.13 ms, `Shadow.LocalMask` 2.05 and 4.24 ms, `FSR31.Upscale` 1.85
   and 1.93 ms, `GBuffer.Resolve.Fullscreen` 1.10 and 1.13 ms,
   `AO.Evaluate` 1.01 and 0.99 ms. Bistro has no froxel fog volume, so the
   Media line has no `Froxel.*` pass.

## Evidence needed

- Release from the repository wrapper, with the graphics validation
  variables and the gate variables above unset, capture-free cases and one
  GPU process on the host.
- `performance-windowed-gpu-submission-vulkan` for the frame target and
  `performance-windowed-gpu` with `--pass-groups` for the lines: five
  children, clean tree, stable warmup, immediate present, exclusive GPU
  lane. Peer sessions share this checkout and keep it dirty, so the
  authoritative runs need a side worktree at the commit under test.
- Matched environment, workload and policy fingerprints; report digests,
  median, p95 and spread across children; `system_conditions` per report.
- Bistro only.

## Risks

- One device sets the budget; RDNA 3, Ampere and newer GPUs have no native
  evidence (ADR-083), so the budget does not transfer between vendors.
- The orbit and walk may miss content-driven peaks, such as many lights in
  view; the binning and shadow proposals need their own cases.
- The cases copy the High preset field by field. When
  `graphics_preset_gates` changes High, the two cases must change with it.
- A hidden window never reaches the display; the presentation cost of a
  visible full-screen window is not in the measurement.
- The walk's rejected surface at frame 319 blocks a passing walk report
  until it is explained.
