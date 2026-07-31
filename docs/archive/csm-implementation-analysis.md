---
status: investigation
updated: 2026-07-31
authority: investigation
---

> **Archived.** Superseded by [`../rendering/cascading-shadow-mapping-design.md`](../rendering/cascading-shadow-mapping-design.md). Retained for history; do not treat as current.
# CSM Implementation Analysis (Current State)

This document captures the renderer’s Cascaded Shadow Mapping (CSM)
implementation as it exists today, the practical tuning knobs that matter most,
and a short list of high-value future improvements.

## Current implementation (Jan 2026)

### Architecture & data flow

- **CPU shadow system**: `lib/src/renderer/systems/vkr_shadow_system.c/h`
  - Computes cascade splits and per-cascade light-space `view_projection`.
  - Allocates per-frame sampled depth textures + render targets.
- **Shadow pass rendering**: `lib/src/renderer/passes/vkr_pass_shadow.c`
  - Depth-only rendering using `assets/shaders/shadow.slang`.
- **World sampling**: `assets/shaders/default.world.slang`
  - Cascade selection + robust coverage checks.
  - Poisson PCF filtering using comparison sampling.
- **Bindings**: `lib/src/renderer/systems/vkr_material_system.c`
  - Binds a single depth array texture to instance sampler `shadow_map`.
- **Vulkan backend**:
  - Depth format selection: `lib/src/renderer/vulkan/vulkan_device.c`
  - Sampled depth attachment creation: `lib/src/renderer/vulkan/vulkan_backend.c`
  - Pipeline state: `lib/src/renderer/vulkan/vulkan_pipeline.c`

### Depth format & sampling requirements

CSM requires a depth format that supports both:
- `VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT`
- `VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT`

On Apple Silicon + MoltenVK, the current selection is:
- `VK_FORMAT_D32_SFLOAT` with `linear_filter=1` (confirmed via startup log)

### Cascade fitting & stabilization (CPU)

Key behaviors:
- **Splits**: log/linear blend controlled by `cascade_split_lambda`.
- **XY bounds**:
  - Uses slice frustum corners to fit bounds.
  - Optionally enforces **constant cascade size** (bounding sphere) to reduce
    “breathing” when rotating relative to the light.
  - Uses a **guard band** (in texels) to reduce coverage pop-in.
- **Z bounds**:
  - Extended using a **scene AABB** to include off-frustum casters and avoid
    camera-move pop-in.
- **Stabilization**:
  - Snaps to the texel grid.
  - Snaps the ortho bounds (not just the center) to reduce shimmer on thin
    geometry.

### Shadow pass depth bias (Vulkan, dynamic)

The shadow pass uses **Vulkan rasterization depth bias** (`vkCmdSetDepthBias`)
to reduce self-shadowing acne (a common source of shimmer on pillars and thin
meshes). This is caster-side *rasterization* bias (Vulkan pipeline state), not
receiver-side shader bias.

Implementation details:

- **Config-driven**: bias values live in `VkrShadowConfig`:
  - `depth_bias_constant_factor`
  - `depth_bias_slope_factor`
  - `depth_bias_clamp`
- **Dynamic state**: the shadow pipeline enables `depthBiasEnable = VK_TRUE` and
  uses `VK_DYNAMIC_STATE_DEPTH_BIAS`, so the values can be changed without
  recreating pipelines.
- **Applied per pass**: `vkr_pass_shadow.c` sets bias immediately after binding
  the shadow pipeline and resets it to zero at the end of the pass to avoid
  leaking state into subsequent passes.
- **Backend path**: `vkr_renderer_set_depth_bias()` forwards into the Vulkan
  backend, which calls `vkCmdSetDepthBias()` on the active graphics command
  buffer.

### World sampling (shader)

In `assets/shaders/default.world.slang`:
- Uses `SamplerComparisonState` + `SampleCmpLevelZero` for shadow map sampling.
- Filtering is **Poisson disk PCF** (16 taps).
- The effective PCF radius is expressed in **texels** (`shadow_pcf_radius`) and
  is scaled per cascade based on `shadow_world_units_per_texel` to keep
  softness roughly consistent in world space.
- Coverage logic is designed to eliminate seams:
  - Deterministic nearest-cascade fallback.
  - PCF-kernel-aware coverage rejection near shadow-map edges.
  - Soft fallback for tiny numerical gaps (prevents 1px holes).

## Configuration presets

Defined in `lib/src/renderer/systems/vkr_shadow_system.h`:

- **`VKR_SHADOW_CONFIG_HIGH`** (project default)
  - `cascade_count = 4`
  - `shadow_map_size = 4096`
  - `pcf_radius = 1.5`
  - `depth_bias_constant_factor = 1.25`
  - `depth_bias_slope_factor = 1.75`
  - `depth_bias_clamp = 0.0`
  - `cascade_blend_range = 8`

- **`VKR_SHADOW_CONFIG_BALANCED`**
  - `cascade_count = 4`
  - `shadow_map_size = 2048`
  - `pcf_radius = 2.0`
  - `depth_bias_constant_factor = 1.50`
  - `depth_bias_slope_factor = 2.00`
  - `depth_bias_clamp = 0.0`
  - `cascade_blend_range = 8`

## Debugging & validation

Useful shader debug modes (see `g_ubo.shadow_debug_mode`):
- **2**: shadow factor (black=shadow, white=lit)
- **4..7**: raw shadow map visualization per cascade
- **8**: coverage diagnostics (useful for seam debugging)

When diagnosing “weird transitions”:
- Check modes **4..7** to confirm each cascade contains real depth data.
- Use mode **8** to confirm coverage fallback is selecting a reasonable cascade
  near boundaries.

## Practical tuning guidance

### Blocky shadows / aliasing

Dominated by shadow map resolution + cascade count. On Apple Silicon, quality
improves significantly going from 1024 → 2048 → 4096.

### Shimmer (especially on pillars)

Dominated by:
- depth bias (Vulkan rasterization + shader receiver bias),
- stabilization (texel snapping),
- and resolution.

### Far-distance “transition” artifacts

Usually a combination of:
- resolution/precision limits,
- cascade coverage rejection near borders (PCF-kernel-aware),
- and split layout (max shadow distance / split lambda).

## Future improvements (high-value)

- **Auto-tune depth bias**:
  - optionally scale constant/slope bias based on cascade world-units-per-texel
    or scene scale to reduce manual tuning across scenes.
- **Optional 32-tap Poisson**:
  - better quality at similar softness, higher cost.
- **PCSS (contact hardening)**:
  - more physically plausible penumbra, significantly more expensive.
- **Dynamic quality tiering**:
  - Balanced vs High based on platform/thermal state.
- **Per-scene shadow settings**:
  - allow overriding scene bounds and max shadow distance per scene.

## Historical note

Earlier versions exhibited camera-tracking seams and shadow pop-in. These were
resolved by:
- extending light-space Z bounds using a scene AABB,
- deterministic cascade fallback,
- comparison sampling + Poisson PCF,
- PCF-kernel-aware edge coverage rejection (per-cascade),
- and texel-grid snapping of ortho bounds.
