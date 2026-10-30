---
status: implemented
updated: 2026-10-30
authority: adr
---

# ADR-139: Desktop deferred lighting register budget

## Status

Accepted. The desktop pipeline's deferred lighting kernels
([deferred.slang](../../renderer/src/shaders/vulkan/slang/world/deferred.slang),
selected in [vkr_vulkan_deferred.c](../../renderer/src/vulkan/vkr_vulkan_deferred.c))
compile the rectangle lights' loop only into the kernels a frame with
rectangle lights takes, and the tables the lighting reads behind a count are
never null ([vkr_vulkan_draws.c](../../renderer/src/vulkan/vkr_vulkan_draws.c)).
Vulkan only; the tiled pipeline's counterpart is
[ADR-138](138-tiled-opaque-register-budget.md).

## Context

`Lighting.Deferred.Fullscreen` is the Lighting line of the desktop budget
([ADR-087](087-gpu-class-graphics-pipelines.md), 4.0 ms p95). At `1d92d4be`
it took 3.624 / 3.781 ms (p50 / p95) on
`bistro_vulkan_high_orbit_1440` and 3.692 / 3.838 ms on
`bistro_vulkan_high_street_walk_1440` (RX 6700 XT, driver 26.10.1,
`performance-windowed-gpu`, authoritative, 5 repetitions each).

Radeon GPU Analyzer prints no registers for this SPIR-V (buffer pointers,
bindless), so counts come from the driver's executable statistics through
`VKR_HARNESS_SHADER_STATISTICS=1` ([ADR-051](051-renderer-harness-and-evidence.md),
[ADR-130](130-performance-window-tabs.md)). The AMD driver runs every compute
pipeline at wave size 64. An RDNA2 SIMD then holds 512 vector registers per
lane slot and at most 16 waves: 154 VGPRs give 3 waves, 128 give 4, 96
give 5.

| Kernel at `1d92d4be` | VGPRs | SGPRs | Scratch bytes | Waves |
|---|---|---|---|---|
| `vk_deferred_lighting` | 154 | 102 | 13,008 | 3 |
| `vk_deferred_lighting_layered` | 256 | 103 | 22,272 | 2 |
| `vk_deferred_lighting_fallback` | 235 | 102 | 13,520 | 2 |
| `vk_deferred_lighting_sample1` | 229 | 100 | 13,520 | 2 |
| `vk_gbuffer_resolve`, all 8 variants | 128 | 103 | 0 | 4 |
| `vk_local_shadow_mask` variants | 123 to 128 | 86 to 102 | 0 | 4 |
| `vk_gtao_evaluate` | 48 | 72 | 0 | 10 |

Twenty copies of `vk_deferred_lighting`, each with one part removed,
located the base kernel's peak:

| Removed | VGPRs | Scratch bytes |
|---|---|---|
| nothing | 154 | 13,008 |
| the rectangle lights' loop | 128 | 0 |
| the sun, or only its shadow | 150, 152 | 6,864 |
| the punctual loop, its lobes, the baked lamps or subsurface | 256 | 0 |
| the environment | 252 | 0 |
| any other part (volume, planar reflection, geometric normal, roughness filter, mask texel, sun contact, GTAO, the punctual mask lookup, contribution measure, wave bounds or cutoff) | 151 to 154 | 13,008 |

The kernel needs more than 256 registers. The compiler then spills 13 KB to
keep 3 waves; with any part removed that brings it under 256 it takes 256
registers and 2 waves without spills, and only without the rectangle loop
does it fit 128. A frame without rectangle lights never runs that loop: its
count is zero and the loop returns at once, yet it holds the registers.
Bistro has no rectangle lights.

## Decision

1. **No-rectangle kernels.** `vk_deferred_shade` takes a compile-time
   `Rectangles` flag beside `Layered` and `MaskFallback`. Each lighting
   kernel has an entry with the loop and one without it:
   `vk_deferred_lighting_no_rectangles`, `_layered_no_rectangles`,
   `_fallback_no_rectangles` and `_sample1_no_rectangles`, sharing their
   bodies through generic helpers. `vkr_vk_prepare_deferred_lighting` takes
   the entries without the loop when the frame's rectangle light count is
   zero, the count the shaders read from the frame's LTC record.
   `VKR_DEFERRED_NO_RECTANGLES_DISABLED=1`, read at renderer creation, keeps
   every frame on the kernels with the loop for diagnosis.
2. **No null table behind a count.** The AMD 26.10.1 compiler may read a
   table ahead of the test that guards it. The frame's LTC record points at
   one zero row when it has no rectangle lights, the sky record at a zero
   cloud chain of one texel per face until `Clouds.SkyLight` patches its
   chain in, and the probe relight factors at a table of ones when no trace
   relit them ([ADR-119](119-dynamic-diffuse-volume-layer.md)), as the light
   rows and decals already did.
3. **Counted changes.** A change to the deferred lighting kernels counts the
   affected kernels' registers before and after with
   `VKR_HARNESS_SHADER_STATISTICS`, and keeps one that raises them only with
   a measured gain.

## Consequences

- On frames without rectangle lights the base kernel uses 128 VGPRs and no
  scratch (4 waves), the layered kernel 256 without its 22 KB of scratch, and
  the fallback and sample1 kernels 185 without scratch. Frames with
  rectangle lights keep the earlier kernels and counts.
- Four more compute pipelines: 16.8 s more of cold compile time (2.2, 8.2,
  3.2 and 3.2 s), none with a warm pipeline cache.
- The layered kernel still holds 2 waves, and the G-buffer resolve and
  `Shadow.LocalMask` 4.
- `frame.ibl_probes` is still zero on a frame whose probes have not
  published, behind `ibl_probe_count`; it is the remaining table of decision
  2's kind.

## Alternatives considered

- **The runtime early return alone.** The loop already returned at once for
  a zero count; registers are allocated for the code a kernel contains, not
  the code it runs.
- **Removing another part.** Every other single removal kept the kernel above
  128 VGPRs (151 to 256), so none reaches 4 waves alone.

## Revisit when

- Scenes commonly carry rectangle lights; then the loop's own registers are
  the target.
- The wave size is measured (wave32 against wave64 needs
  `subgroupSizeControl`, which the device does not enable yet).
- A driver changes the spill or wave-size heuristics; recount the kernels.

## Evidence

RX 6700 XT, AMD driver 26.10.1, Windows 10 19045, Release, Bistro
`bistro_desktop_lamps_local` (baked lamps), 2560×1440, 2026-10-30.

- Baseline: the two ADR-087 cases above with
  `--pass-groups tools/budgets/desktop.pass_groups.json`, reports
  `sha256:a167a12c…` (orbit) and `sha256:bf729053…` (walk).
- Peak map: each ablated copy compiled alone with
  `vkr_bakery shaders --entry packet.deferred_lighting.comp` into its own
  catalog (`VKR_SHADER_CATALOG`), unmodified copy byte-identical to the
  build's, counted on the orbit settings with
  `VKR_HARNESS_SHADER_STATISTICS=1`.
- Timing, one binary, `VKR_DEFERRED_NO_RECTANGLES_DISABLED=1` against unset,
  orbit, `local-windowed-gpu-single`, 3 alternating rounds:
  `Lighting.Deferred.Fullscreen` p50 3.625 to 3.629 against 2.995 to
  2.997 ms, p95 3.781 to 3.785 against 3.117 to 3.124 ms (−0.63 ms);
  `frame.wall` p50 13.08 to 13.09 against 12.47 to 12.55 ms;
  `GBuffer.Resolve.Fullscreen` unchanged at 1.29 ms.
- Image: `hdr_pre_transmission` and depth snapshots at frame 120 of the
  orbit. All five runs without the variant and four of seven with it
  captured identical bytes (`769abbf95fce…`, depth `0aa539151a44…` in
  every run); the other three runs with it differ in 512 pixels of foliage
  seen through a window by at most 0.0016 (PSNR 113.9 dB, no pixel over
  2/255), one same result each time.
- Device loss: before decision 2, two of three snapshots with the variant
  lost the device (`invalid read at 0x0`, within 4 KB); snapshots without it
  and every profile run passed. GPU-assisted validation and synchronization
  validation each ran one snapshot with the variant clean and without a
  loss. With `probe_relight` alone non-null a snapshot still lost the device.
  With all three tables non-null, 5 of 5 snapshots with the variant and 3 of
  3 without it passed. The transmission kernels, which read the rectangle
  rows, compile to identical SPIR-V with and without the variant, so the
  table the variant's code read ahead of its test is the cloud chain, null
  on frames where `Clouds.SkyLight` returns before patching it.
