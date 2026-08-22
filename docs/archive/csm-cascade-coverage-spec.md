---
status: superseded
updated: 2026-08-22
authority: spec
---
# CSM Cascade Coverage Spec (Sharp → Loose)

> **Superseded by**
> [Shadow CPU Cost and CSM Rewrite Spec](../rendering/shadow-cpu-cost-and-csm-rewrite-spec.md).
> The per-cascade CPU guard-band and Z-extension controls remain active, but the
> proposed UV-margin packet fields had no shipped receiver and were removed by
> that spec's truthful-contract phase.

## Summary
Define per-cascade coverage controls so near cascades are tighter (sharper) and
far cascades are looser (more tolerant of edge/coverage issues). The goal is to
reduce shimmering and edge seams while preserving sharpness near the camera.

This document is a spec with implementation notes; the checklist is updated
as items land.

## Goals
- Allow per-cascade tuning of *coverage strictness* in the shader.
- Allow per-cascade tuning of *world-space coverage* in the CPU cascade fit.
- Provide a clear default profile: **cascade 0 sharpest → cascade N-1 loosest**.
- Maintain backward compatibility with existing configs by keeping sensible
  defaults and clamping rules.

## Non-Goals
- Changing the number of cascades or the overall split strategy.
- Reworking the render graph or shadow pass scheduling.
- Changing light-space stabilization or snapping behavior.

## Background (Current Behavior)
- Cascade coverage in the shader is controlled by:
  - `shadow_uv_margin()` (strict coverage)
  - `shadow_uv_soft_margin()` (soft fallback)
  - `shadow_uv_kernel_margin()` (PCF kernel edge safety)
  - `shadow_cascade_blend_range` (blend width between cascades)
- CPU coverage is controlled by:
  - `cascade_guard_band_texels` (XY expansion)
  - `z_extension_factor` or `scene_bounds` (Z expansion)
  - `use_constant_cascade_size` (fit behavior)

Currently, these are *global* (apply equally to all cascades).

## Proposed Behavior
Introduce per-cascade controls that follow a *sharp → loose* profile:
- **Cascade 0 (closest)**: most strict coverage, smallest margins.
- **Farthest cascade**: most lenient coverage, largest margins.

### A. Shader Coverage Controls (Per-Cascade)
Add per-cascade scalar multipliers (or absolute values) to scale the existing
coverage margins:

- `shadow_uv_margin_scale[i]` (strict coverage margin)
- `shadow_uv_soft_margin_scale[i]` (soft fallback margin)
- `shadow_uv_kernel_margin_scale[i]` (edge safe region for PCF)

Default policy (example profile):
- `shadow_uv_margin_scale`: [0.75, 1.0, 1.25, 1.5] for 4 cascades
- `shadow_uv_soft_margin_scale`: [1.0, 1.25, 1.5, 2.0]
- `shadow_uv_kernel_margin_scale`: [1.0, 1.0, 1.1, 1.2]

These scales multiply the existing computed margins per cascade:
- strict margin: `shadow_uv_margin(c) * scale`
- soft margin: `shadow_uv_soft_margin(c) * scale`
- kernel margin: `shadow_uv_kernel_margin(c) * scale`

### B. CPU Coverage Controls (Per-Cascade)
Add per-cascade expansion controls to the cascade fit:

- `cascade_guard_band_texels[i]` (XY expansion per cascade)
- `cascade_z_extension_factor[i]` (Z extension per cascade; used when
  `scene_bounds.use_scene_bounds == false`)

Default policy (example profile):
- `cascade_guard_band_texels`: [64, 96, 128, 160]
- `cascade_z_extension_factor`: [3.0, 4.0, 5.0, 6.0]

### C. Blending
Keep `shadow_cascade_blend_range` global by default. Optionally define:
- `shadow_cascade_blend_range[i]` per cascade if finer control is desired.

## Data Flow & Layout

### Config (CPU)
Add optional per-cascade arrays to `VkrShadowConfig`:
- `float32_t cascade_guard_band_texels_per[VKR_SHADOW_CASCADE_COUNT_MAX]`
- `float32_t cascade_z_extension_factor_per[VKR_SHADOW_CASCADE_COUNT_MAX]`
- `float32_t shadow_uv_margin_scale_per[...]`
- `float32_t shadow_uv_soft_margin_scale_per[...]`
- `float32_t shadow_uv_kernel_margin_scale_per[...]`

Rules:
- Per-cascade arrays treat `0` as “unset”.
- If any per-cascade values are provided for a field, zero entries fall back to
  the existing global value (guard band / z-extension) or `1.0` (UV scales).
- If no per-cascade values are provided for a field, use the default
  sharp→loose profile described below.
- Clamp negatives to 0.0.
- Limit cascade count to `VKR_SHADOW_CASCADE_COUNT_MAX`.

### Frame Data (CPU → GPU)
Extend `VkrShadowFrameData` to include packed per-cascade scales:
- `shadow_uv_margin_scale[8]`
- `shadow_uv_soft_margin_scale[8]`
- `shadow_uv_kernel_margin_scale[8]`

Pack each into two `vec4` (like existing shadow arrays) for the UBO:
- `shadow_uv_margin_scale[2]`
- `shadow_uv_soft_margin_scale[2]`
- `shadow_uv_kernel_margin_scale[2]`

### Shader (GPU)
Add uniform arrays and apply to margin calculations:
- `shadow_uv_margin(c) * scale`
- `shadow_uv_soft_margin(c) * scale`
- `shadow_uv_kernel_margin(c) * scale`

## Defaults

Provide a built-in default profile for sharp → loose (for 4 cascades):
- `uv_margin_scale`: [0.75, 1.0, 1.25, 1.5]
- `soft_margin_scale`: [1.0, 1.25, 1.5, 2.0]
- `kernel_margin_scale`: [1.0, 1.0, 1.1, 1.2]
- `guard_band_texels`: [64, 96, 128, 160]
- `z_extension_factor`: [3.0, 4.0, 5.0, 6.0]

These defaults are only applied when per-cascade values are unset (zero). The
guard-band and z-extension defaults are applied as a **scale** on the existing
global values so legacy configs keep their overall magnitude.

## Debugging & Validation
Add optional debug modes or logging to verify:
- Packed per-cascade values are non-zero and ordered correctly.
- Visualize cascade margins (e.g., debug overlays of strict vs soft coverage).
- Compare with existing behavior by setting all scales to 1.0.

## Compatibility / Risks
- **UBO layout** changes must be mirrored in `default.world.shadercfg` to avoid
  offset corruption.
- Per-cascade arrays increase UBO size; verify against any alignment or size
  limits.
- Ensure per-cascade values are filled for unused cascades (set to 0 or 1 as
  appropriate) to avoid undefined values.

## Implementation Checklist (for future work)
- [x] Add per-cascade fields to `VkrShadowConfig` and `VkrShadowFrameData`.
  Impl: Added per-cascade arrays and clamping in
  `lib/src/renderer/systems/vkr_shadow_system.h` and
  `lib/src/renderer/systems/vkr_shadow_system.c`.
- [x] Populate per-cascade values in `vkr_shadow_system_update()` and
      `vkr_shadow_system_get_frame_data()`.
  Impl: Guard band and z-extension are computed per cascade in
  `lib/src/renderer/systems/vkr_shadow_system.c`. UV margin scales are
  generated per cascade in frame data with default profiles when unset.
- [x] Pack per-cascade arrays into `vec4[2]` in
      `vkr_pass_world_apply_shadow_globals()`.
  Impl: Packed `shadow_uv_*_scale` arrays in
  `lib/src/renderer/passes/vkr_pass_world.c`.
- [x] Add new uniforms to `assets/shaders/default.world.shadercfg` in correct
      order.
  Impl: Added `shadow_uv_margin_scale`, `shadow_uv_soft_margin_scale`, and
  `shadow_uv_kernel_margin_scale` in UBO order.
- [x] Update `lib/src/renderer/vulkan/shaders/common/csm.slangh` to use the scales.
  Impl: Applied the scales to strict/soft/kernel margin calculations.
- [ ] Recompile shaders and validate with debug modes.
