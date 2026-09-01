---
status: implemented
updated: 2026-09-01
authority: adr
---

# ADR-039: Metal internal scene scale with folded spatial upscale

## Status

**Accepted.** Metal supports an immutable internal scene scale in `(0, 1]`.
Scale `1.0` remains the default. Vulkan rejects non-unit scale, and non-unit
scale rejects editor compositor mode.

This is an explicit quality and performance mode. It is not a native-resolution
optimization, MetalFX integration, or temporal upscaler.

## Context

The production-effects Bistro orbit on the owner M1 Pro resolved a 1280x720
logical hidden window to a 2560x1440 Metal target. At native scene resolution,
the local Release observation measured 30.750 ms mean and 34.765 ms p95. Xcode
Metal System Trace and renderer timestamps put the dominant work inside deferred
lighting, G-buffer resolve, temporal resolve, transmission, and other
pixel-scaled shaders. CoreAnimation reported no client-buffer wait intervals.

Two same-quality shader changes reduced measured means by about 0.25 ms in
total. Metal G-buffer and transmission resolve now derive center barycentrics
and analytical gradients from one homogeneous solve, and deferred lighting
hoists the view-side geometry term out of the punctual-light loop. These are
useful but far short of the roughly 21 ms native p95 deficit to 75 FPS with all
production effects enabled.

The presentation specification already required separate output and internal
extents. Scaling the physical target would have broken DPI, UI, capture, and
picking semantics. The remaining choice was to keep native output and make the
scene scale visible as a declared workload control.

## Decision

1. `ApplicationConfig.render_scale` and
   `VkrRendererBackendConfig.render_scale` are immutable initialization
   controls. Zero selects `1.0`. The frontend accepts finite values in `(0, 1]`
   only, and accepts non-unit values only for Metal.
2. The present target keeps its physical extent. Metal derives the scene extent
   by rounding `output_extent * render_scale` to the nearest integer with a
   minimum of one pixel. The harness records both extents from renderer device
   information.
3. Render-graph resources authored with `extent:{mode:viewport}` use the scene
   extent. The two fullscreen HDR resources that previously used `window` now
   use `viewport`, so visibility, depth, G-buffer, lighting, transmission,
   bloom, GTAO, exposure, HZB, and temporal work stay in one internal pixel
   domain.
4. Metal `Post.Tonemap` keeps the physical target viewport. Its 32-byte root
   carries `output_extent` at offset 24. The fragment maps output pixel centers
   to normalized UV, linearly samples the internal HDR source, and applies FXAA
   offsets in output pixels. This folds the spatial upscale into the existing
   final draw. UI and text still compose at output resolution.
5. TAA remains same-resolution inside the scene domain. A scale or extent
   change resets its history through the existing extent rule. This decision
   does not describe the linear sample as temporal reconstruction.
6. Fullscreen picking maps physical output coordinates to internal pixels with
   the renderer's existing edge-to-edge convention. The editor owns a different
   dock-panel mapping, and scaled editor composition is outside this decision,
   so the renderer cancels and rejects an editor packet when scale is not
   `1.0`. The harness rejects the same combination before launch.
7. Harness manifests may author `renderer.render_scale`. A non-default value
   changes the workload fingerprint, reports include `resolution`,
   `render_resolution`, and `render_scale`, and capture-summary version 5 keeps
   the effective renderer extent. Old summaries load with scale `1.0`.

## Consequences

- Scale `1.0` preserves the old target and scene dimensions, graph topology,
  and historical workload fingerprints.
- Vulkan still realizes `viewport` at the window extent because it accepts only
  scale `1.0`; the shared graph edit does not lower Vulkan resolution.
- The mode reduces scene detail. At the fixed Bistro camera, the final
  2560x1440 scale-0.4 image compared with the native image at SSIM `0.971152`
  and PSNR `38.960 dB`. This is a useful performance setting, not a visual
  equivalence claim. No accepted image baseline changed.
- The final dirty-tree local five-process production orbit measured 9.176 ms
  mean and 11.931 ms p95 across 1,500 frames, about 109.0 average FPS and 83.8
  FPS at p95. Every child mean stayed below 9.23 ms and the slowest child p95
  was 12.326 ms. The output was 2560x1440 and the renderer-reported scene extent
  was 1024x576. Report digest:
  `sha256:3a513d7118ced135611c91afe02c61d9787c22c675f4daa4f74bbe13a70e6940`.
- That result is not authoritative. The local profile, dirty implementation,
  warmup instability, and moving transmission/texture work-volume mismatch are
  recorded authority failures. A clean accepted profile must rerun after the
  change lands before this becomes a baseline claim.
- The five-process timestamp-on diagnostic measured a 6.261 ms sum across
  supported GPU pass scopes. It is diagnostic because timestamp overhead
  changes frame time and ten transfer/UI scopes remain explicitly unsupported.
  Report digest:
  `sha256:eb2be3217543ad3792ed7b0e8c5ce544b1a683a0a8c9b196f0bab813f0df3ba7`.
- A post-review Debug diagnostic at 640x480 output and 256x192 scene extent
  passed with Metal API validation and Metal GPU shader validation enabled in
  one serialized process. The only stderr lines announce the two validators;
  no validation, sanitizer, assertion, overflow, or resolve-invalid failure
  occurred. Report digest:
  `sha256:3421ff468abf21de59570bb4dc7c4ea3d7b04292a09277cbd445ff8ec1219afc`.

## Alternatives considered

### Keep native scene resolution and optimize shaders only

Retained as ongoing work, but rejected as the way to claim this device target.
The measured same-quality wins were about 0.25 ms against a native production
p95 deficit of roughly 21 ms.

### Disable production effects or reduce shadows

Rejected. Automatic exposure, bloom, GTAO, TAA, four 2048-pixel cascades, and
the default render mode remain enabled in the target case. Changing them would
measure a different production workload without isolating one declared control.

### Add MetalFX in the same decision

Deferred from this decision. MetalFX needed a separate motion, depth, reset,
quality, lifetime, and cross-backend policy. ADR-040 now implements that
separate Metal-only strategy; ADR-039's spatial mode remains the default.

### Scale the physical target

Rejected. It would lie about the present target, lower UI and text resolution,
change capture dimensions, and conflict with the DPI contract.

## Revisit when

- ADR-040's MetalFX strategy gains accepted moving-image quality evidence;
- the editor viewport consumes packet-authored dimensions and has a correct
  output-to-panel-to-scene picking chain;
- Vulkan needs the same control and has a chosen upscale algorithm plus native
  validation; or
- owner quality review rejects scale `0.4` for the Bistro target.
